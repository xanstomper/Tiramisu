// tiramisu/src/host/main.cpp
// Tiramisu host — the UWP "Game" app for Xbox Series Dev Mode that:
//   * boots the trt runtime and executes Windows PE64s in-process
//   * serves the phone transfer page (QR on TV -> upload game zips -> launch)
//   * feeds guests keyboard/mouse (CoreWindow events) + controller (XInput),
//     including gamepad-as-mouse for KBM-only games
//   * optional Steam QR login (own account; depot pipeline is M6)
#include "host_log.h"
#include "audio.h"
#include "swapchain.h"
#include "ui.h"
#include "webxfer.h"
#include "settings.h"
#include "../trt/trt.h"
#include "../trt/bridge_res.h"
#include "../trt/bridge_nvapi.h"
#include "../trt/iso.h"
#include "../trt/input.h"
#include "../trt/images.h"
#include "../trt/bridge_mem.h"
#include "../trt/bridge_d3d.h"
#include "../trt/bridge_openal.h"
#include "../trt/bridge_reg.h"

#include <winrt/base.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Input.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.Networking.h>
#include <winrt/Windows.Networking.Connectivity.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.Devices.Input.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/Windows.Web.Http.h>

#include <d2d1_1.h>
#include <dwrite.h>
#include <d3d11.h>
#include <xinput.h>
#include <winrt/Windows.UI.Input.h>
#include "native_gameinput.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "xinput.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

using namespace winrt;
using namespace winrt::Windows::ApplicationModel::Core;
using namespace winrt::Windows::UI::Core;

namespace {

std::string g_local_state;
std::string g_games_dir;
std::mutex g_status_mu;
std::string g_guest_status = "idle";
std::string g_steam_status = "press Y to link Steam (your own account)";
std::string g_steam_qr;  // URL to render as QR (steam or transfer)
std::atomic<bool> g_steam_busy{false};
std::atomic<bool> g_shutdown{false};
std::atomic<bool> g_guest_running{false};
DWORD g_ui_tid = 0;
std::thread g_guest_thread;
std::string g_pending_launch;
bool g_mouse_mode = false;
bool g_kbd_mouse_mode = false;
bool g_quick_menu_open = false;
std::string g_control_code_cached;
std::atomic<bool> g_f8_down{false};
std::atomic<bool> g_f8_pressed{false};
std::atomic<ULONGLONG> g_f8_edge_ms{0};
std::atomic<bool> g_km_keys[256]{};
bool g_booted = false;
bool g_setup_done = false;      // first-run setup screen flag
bool g_setup_active = false;    // currently showing the setup screen

// dashboard settings screen state (rows mirror longan::host_settings;
// the string/int buffers are edited here, applied via settings_store)
longan::ui_setting g_set_rows[18];
int g_set_count = 0;
int g_set_selected = 0;
longan::ui_screen g_screen = longan::UI_SHELF;

void set_guest_status(std::string s) {
  std::lock_guard<std::mutex> lk(g_status_mu);
  g_guest_status = std::move(s);
}

std::string get_guest_status() {
  std::lock_guard<std::mutex> lk(g_status_mu);
  return g_guest_status;
}

// ---------------- helpers ----------------
std::string ws2u8(winrt::hstring const& h) {
  const std::wstring w(h.c_str());
  std::string s;
  for (wchar_t c : w) s.push_back(c >= 0x20 && c < 0x7F ? (char)c : '?');
  return s;
}

std::string ws2u8(const std::wstring& w) {
  std::string s;
  for (wchar_t c : w) s.push_back(c >= 0x20 && c < 0x7F ? (char)c : '?');
  return s;
}

std::string str2u8(const std::string& s) { return s; }
std::wstring u82ws(const std::string& s) { return std::wstring(s.begin(), s.end()); }

std::string package_install_path() {
  try {
    return ws2u8(winrt::Windows::ApplicationModel::Package::Current()
                     .InstalledLocation()
                     .Path());
  } catch (...) {
    return {};
  }
}

std::string lan_ip() {
  using namespace winrt::Windows::Networking::Connectivity;
  try {
    const auto hosts = NetworkInformation::GetHostNames();
    for (unsigned i = 0; i < hosts.Size(); ++i) {
      const auto h = hosts.GetAt(i);
      if (h.Type() == winrt::Windows::Networking::HostNameType::Ipv4) {
        const std::string ip = ws2u8(h.DisplayName());
        if (ip.rfind("10.", 0) == 0 || ip.rfind("192.168.", 0) == 0 ||
            ip.rfind("172.", 0) == 0)
          return ip;
      }
    }
    if (hosts.Size() > 0) return ws2u8(hosts.GetAt(0).DisplayName());
  } catch (...) {
  }
  return "?";
}

// ---------------- UWP memory backend (W^X: no RWX, ever) ----------------
void* uwp_alloc(size_t bytes, u64 preferred_base) {
  (void)preferred_base;  // AppContainer cannot honor fixed bases
  return VirtualAllocFromApp(nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                             PAGE_READWRITE);
}
void* uwp_reserve(size_t bytes, u64 preferred_base) {
  (void)preferred_base;
  return VirtualAllocFromApp(nullptr, bytes, MEM_RESERVE, PAGE_NOACCESS);
}
int uwp_commit(void* base, size_t bytes, unsigned prot) {
  DWORD fl = PAGE_READONLY;
  if ((prot & (TRT_PROT_W | TRT_PROT_X)) == (TRT_PROT_W | TRT_PROT_X))
    fl = PAGE_EXECUTE_READWRITE;
  else if (prot & TRT_PROT_X)
    fl = PAGE_EXECUTE_READ;
  else if (prot & TRT_PROT_W)
    fl = PAGE_READWRITE;
  return VirtualAllocFromApp(base, bytes, MEM_COMMIT, fl) ? 0 : 1;
}
int uwp_decommit(void* base, size_t bytes) {
#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable: 6250)
#endif
  return VirtualFree(base, bytes, MEM_DECOMMIT) ? 0 : 1;
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif
}
int uwp_protect(void* base, size_t bytes, unsigned prot) {
  DWORD fl = PAGE_READONLY;
  if ((prot & (TRT_PROT_W | TRT_PROT_X)) == (TRT_PROT_W | TRT_PROT_X))
    fl = PAGE_EXECUTE_READWRITE;
  else if (prot & TRT_PROT_X)
    fl = PAGE_EXECUTE_READ;
  else if (prot & TRT_PROT_W)
    fl = PAGE_READWRITE;
  DWORD old = 0;
  return VirtualProtectFromApp(base, bytes, fl, &old) ? 0 : 1;
}
void uwp_release(void* base, size_t) { VirtualFree(base, 0, MEM_RELEASE); }

// guest C++ exceptions must unwind: register the .pdata ranges with the OS
int uwp_register_unwind(void* image_base, const void* pdata, u32 count) {
  return RtlAddFunctionTable((PIMAGE_RUNTIME_FUNCTION_ENTRY)pdata, count,
                             (DWORD64)(uintptr_t)image_base)
             ? 0
             : 1;
}

void uwp_log(const char* line) { longan::log_line(line); }

std::atomic<bool> g_uwp_raw_mouse{false};
std::atomic<bool> g_interceptor_kbd_active{false};
longan::native_gameinput g_native_input;
bool g_input_focused = false;

extern "C" u32 TRT_STDCALL trt_MapVirtualKeyW(u32 code, u32 type);
extern "C" u32 trt_vk_to_char_ex(u32 vk, const u8* ks);

bool launcher_key(u16 vk) {
  return trt_key_down(vk) || (trt_key_async(vk) & 1) != 0;
}

inline u16 normalize_numpad_vk(u16 vk, u16 scan, bool ext) {
  switch (vk) {
    case 0x60: case 0x61: case 0x62: case 0x63: case 0x64:
    case 0x65: case 0x66: case 0x67: case 0x68: case 0x69:
    case 0x6B: case 0x6D: case 0x6E:
      return vk;
    default:
      break;
  }
  if (scan && !ext) {
    switch (scan & 0xFFu) {
      case 0x47: return 0x67;  // Numpad 7
      case 0x48: return 0x68;  // Numpad 8
      case 0x49: return 0x69;  // Numpad 9
      case 0x4A: return 0x6D;  // Numpad -
      case 0x4B: return 0x64;  // Numpad 4
      case 0x4C: return 0x65;  // Numpad 5
      case 0x4D: return 0x66;  // Numpad 6
      case 0x4E: return 0x6B;  // Numpad +
      case 0x4F: return 0x61;  // Numpad 1
      case 0x50: return 0x62;  // Numpad 2
      case 0x51: return 0x63;  // Numpad 3
      case 0x52: return 0x60;  // Numpad 0
      case 0x53: return 0x6E;  // Numpad .
      default: break;
    }
  }
  return vk;
}

inline bool is_numpad_cursor_vk(u16 nvk) {
  switch (nvk) {
    case 0x60: case 0x61: case 0x62: case 0x63: case 0x64:  // Numpad 0-4
    case 0x65: case 0x66: case 0x67: case 0x68: case 0x69:  // Numpad 5-9
    case 0x6B: case 0x6D: case 0x6E:                       // Numpad + / - / .
      return true;
    default:
      return false;
  }
}

inline bool is_f8_cursor_vk(u16 nvk) {
  switch (nvk) {
    case 0x25: case 0x26: case 0x27: case 0x28:  // Arrows
    case 0x0D: case 0x20:                        // Enter / Space (LMB)
    case 0xA3: case 0x5D:                        // RCtrl / Apps (RMB)
    case 0x21: case 0x22:                        // PageUp / PageDown (Wheel)
      return true;
    default:
      return false;
  }
}

inline bool handle_host_cursor_key(u16 raw_vk, u16 scan, bool ext, bool down) {
  if (raw_vk == 0x77 /*VK_F8*/) {
    if (down) {
      if (!g_f8_down.exchange(true)) {
        const ULONGLONG now = GetTickCount64();
        if (now - g_f8_edge_ms.load() >= 120) {
          g_f8_edge_ms.store(now);
          g_f8_pressed.store(true);
        }
      }
    } else {
      g_f8_down.store(false);
    }
    return true;
  }
  const u16 nvk = normalize_numpad_vk(raw_vk, scan, ext);
  if (is_numpad_cursor_vk(nvk)) {
    if (nvk < 256) g_km_keys[nvk].store(down);
    return down || !trt_key_down(raw_vk);
  }
  if (!down && nvk < 256) g_km_keys[nvk].store(false);
  if (g_kbd_mouse_mode && is_f8_cursor_vk(nvk)) {
    if (down && nvk < 256) g_km_keys[nvk].store(true);
    return down || !trt_key_down(raw_vk);
  }
  return false;
}

inline bool km_key_held(u16 vk) {
  return (vk < 256 && g_km_keys[vk].load()) || trt_key_down(vk);
}

void refresh_control_hint() {
  const std::string code_part = g_control_code_cached.empty()
      ? std::string()
      : ("  |  /controls " + g_control_code_cached);
  const std::string text = g_kbd_mouse_mode
      ? ("CURSOR ON (Cyan | F8: off | Arrows+Space | Numpad)" + code_part)
      : ("F8: Cursor Mode | Numpad: Mouse" + code_part);
  longan::ui_set_control_hint(u82ws(text));
}

inline u16 disambiguate_modifier_vk(u16 vk, u16 scan, bool ext) {
  if (vk == 0x10 /*VK_SHIFT*/) return (scan == 0x36u || ext) ? 0xA1u : 0xA0u;
  if (vk == 0x11 /*VK_CONTROL*/) return ext ? 0xA3u : 0xA2u;
  if (vk == 0x12 /*VK_MENU*/) return ext ? 0xA5u : 0xA4u;
  return vk;
}

void feed_core_pointer(CoreWindow const& window,
                       winrt::Windows::Foundation::Point point) {
  // Once Xbox delivers relative motion, the drawn cursor owns the position.
  // Absolute events can still report a system pointer stuck at a screen edge.
  if (g_uwp_raw_mouse.load()) return;
  const auto bounds = window.Bounds();
  if (bounds.Width <= 0 || bounds.Height <= 0) return;
  int w, h;
  trt_display_size(&w, &h);
  const int x = std::clamp(static_cast<int>(point.X * w / bounds.Width), 0, w - 1);
  const int y = std::clamp(static_cast<int>(point.Y * h / bounds.Height), 0, h - 1);
  trt_input_pointer_move(x, y, false);
}

inline int host_mouse_btn_for_press(winrt::Windows::UI::Input::PointerUpdateKind k) {
  using PUK = winrt::Windows::UI::Input::PointerUpdateKind;
  if (k == PUK::LeftButtonPressed) return 0;
  if (k == PUK::RightButtonPressed) return 1;
  if (k == PUK::MiddleButtonPressed) return 2;
  if (k == PUK::XButton1Pressed) return 3;
  if (k == PUK::XButton2Pressed) return 4;
  return -1;
}

inline int host_mouse_btn_for_release(winrt::Windows::UI::Input::PointerUpdateKind k) {
  using PUK = winrt::Windows::UI::Input::PointerUpdateKind;
  if (k == PUK::LeftButtonReleased) return 0;
  if (k == PUK::RightButtonReleased) return 1;
  if (k == PUK::MiddleButtonReleased) return 2;
  if (k == PUK::XButton1Released) return 3;
  if (k == PUK::XButton2Released) return 4;
  return -1;
}

void poll_host_gamepads(XINPUT_STATE* pad0_out, bool* pad0_ok_out) {
  for (DWORD idx = 0; idx < 4; ++idx) {
    XINPUT_STATE xs{};
    const bool ok = (XInputGetState(idx, &xs) == ERROR_SUCCESS);
    trt_input_pad_state_n(idx, ok, xs.Gamepad.sThumbLX, xs.Gamepad.sThumbLY,
                          xs.Gamepad.sThumbRX, xs.Gamepad.sThumbRY,
                          xs.Gamepad.wButtons, xs.Gamepad.bLeftTrigger,
                          xs.Gamepad.bRightTrigger);
    if (ok) {
      u16 l = 0, r = 0;
      if (trt_pad_get_vibration(idx, &l, &r)) {
        XINPUT_VIBRATION xv{l, r};
        ::XInputSetState(idx, &xv);
      }
    }
    if (idx == 0) {
      if (pad0_out) *pad0_out = xs;
      if (pad0_ok_out) *pad0_ok_out = ok;
    }
  }
}

// host pump the guest's GetMessageW calls: dispatcher + pad, nothing else
void check_guest_stop_trap();

void host_pump() {
  // CoreWindow/Dispatcher lives strictly on g_ui_tid; background guest threads
  // yield so the UI loop processes messages, XInput, and audio cleanly.
  if (GetCurrentThreadId() != g_ui_tid) {
    check_guest_stop_trap();
    SwitchToThread();
    Sleep(1);
    return;
  }
  CoreWindow::GetForCurrentThread()
      .Dispatcher()
      .ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
  trt_input_frame_begin();
  poll_host_gamepads(nullptr, nullptr);
  trt_openal_pump(8);
}

// ---------------- system-route binder (console real DLLs) ----------------
std::mutex g_sys_mu;

void* system_bind(const char* module, const char* symbol) {
  if (!module || !symbol) return nullptr;
  std::lock_guard<std::mutex> lk(g_sys_mu);
  static wchar_t last_mod[260] = L"";
  static HMODULE h = nullptr;
  wchar_t wmod[260];
  size_t n = 0;
  while (module[n] && n < 258) { wmod[n] = (wchar_t)(unsigned char)module[n]; ++n; }
  wmod[n] = 0;
  if (!h || _wcsicmp(last_mod, wmod) != 0) {
    h = GetModuleHandleW(wmod);
    if (!h) h = LoadLibraryW(wmod);
    wcscpy(last_mod, wmod);
  }
  if (!h) return nullptr;
  LPCSTR sym = symbol;
  if (symbol[0] == '#') {
    char* endp = nullptr;
    const unsigned long ord = strtoul(symbol + 1, &endp, 10);
    if (endp != symbol + 1 && ord > 0 && ord <= 0xFFFFul) {
      sym = (LPCSTR)(uintptr_t)ord;
    }
  }
  return (void*)GetProcAddress(h, sym);
}

// ---------------- smoke probes ----------------
void run_probes() {
  char line[192];
  MEMORYSTATUSEX ms{sizeof(MEMORYSTATUSEX)};
  if (GlobalMemoryStatusEx(&ms)) {
    snprintf(line, sizeof(line), "probe: memory avail=%llu MB / %llu MB",
             (unsigned long long)(ms.ullAvailPhys / (1024 * 1024)),
             (unsigned long long)(ms.ullTotalPhys / (1024 * 1024)));
    longan::log_line(line);
  }
  // G3 JIT capability probes: direct RWX vs W->RX codeGeneration contract
  void* rwx = VirtualAllocFromApp(nullptr, 4096, MEM_COMMIT | MEM_RESERVE,
                                  PAGE_EXECUTE_READWRITE);
  if (rwx) {
    longan::log_line("probe: jit RWX alloc -> ALLOWED");
    VirtualFree(rwx, 0, MEM_RELEASE);
  } else {
    snprintf(line, sizeof(line), "probe: jit RWX alloc -> DENIED (err=%lu)",
             (unsigned long)GetLastError());
    longan::log_line(line);
  }
  void* wx = VirtualAllocFromApp(nullptr, 4096, MEM_COMMIT | MEM_RESERVE,
                                 PAGE_READWRITE);
  if (wx) {
    DWORD oldp = 0;
    const BOOL rx_ok = VirtualProtectFromApp(wx, 4096, PAGE_EXECUTE_READ, &oldp);
    snprintf(line, sizeof(line), "probe: jit W->RX transition -> %s (err=%lu)",
             rx_ok ? "ALLOWED" : "DENIED",
             (unsigned long)(rx_ok ? 0 : GetLastError()));
    longan::log_line(line);
    VirtualFree(wx, 0, MEM_RELEASE);
  }
  const wchar_t* const k_probe_dlls[] = {
      L"d3d11.dll", L"d3dcompiler_47.dll", L"dstorage.dll",
      L"gdi32.dll", L"shlwapi.dll",        L"ws2_32.dll",
      L"bcrypt.dll", L"winmm.dll",
  };
  for (const wchar_t* dn : k_probe_dlls) {
    HMODULE hm = GetModuleHandleW(dn);
    if (!hm) hm = LoadLibraryW(dn);
    snprintf(line, sizeof(line), "probe: dll %ls -> %s", dn,
             hm ? "PRESENT" : "MISSING");
    longan::log_line(line);
  }
  HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
  auto create_proc =
      (decltype(&CreateProcessW))GetProcAddress(k32, "CreateProcessW");
  if (create_proc) {
    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    const BOOL ok = create_proc(L"notepad.exe", nullptr, nullptr, nullptr,
                                FALSE, 0, nullptr, nullptr, &si, &pi);
    snprintf(line, sizeof(line), "probe: CreateProcessW -> %s",
             ok ? "ALLOWED (unexpected!)" : "BLOCKED (expected)");
    longan::log_line(line);
  }
  DWORD pads = 0;
  for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
    XINPUT_STATE st{};
    if (XInputGetState(i, &st) == ERROR_SUCCESS) ++pads;
  }
  snprintf(line, sizeof(line), "probe: gamepads=%lu", (unsigned long)pads);
  longan::log_line(line);
}

// ---------------- game shelf ----------------
struct game_entry {
  std::string name;
  std::string dir;  // full root directory of this game (LocalState or USB)
  std::string exe;  // selected relative path inside game_dir (or the image)
  std::string image;  // disc image (.iso/.bin/.img) when the game ships on one
  std::vector<std::string> exes;
  int exe_idx = 0;
  std::string compat;      // static PE analysis, shown before launch
  std::string input_mode;  // auto | controller | kbm | pad2kbm
};
std::vector<game_entry> g_games;
int g_selected = 0;

std::string lower_ascii(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

bool is_junk_exe(const std::string& fname) {
  const std::string s = lower_ascii(fname);
  return s.find("unitycrashhandler") != std::string::npos ||
         s.find("crashreport") != std::string::npos ||
         s.find("ueprereq") != std::string::npos ||
         s.find("ue4prereq") != std::string::npos ||
         s.find("ue5prereq") != std::string::npos ||
         s.find("vcredist") != std::string::npos ||
         s.find("vc_redist") != std::string::npos ||
         s.find("dxsetup") != std::string::npos ||
         s.find("dotnet") != std::string::npos ||
         s.find("unins0") != std::string::npos ||
         s.find("uninstall") != std::string::npos ||
         s.find("easyanticheat") != std::string::npos ||
         s.find("battleye") != std::string::npos;
}

struct exe_cand {
  std::string rel;
  u64 size;
  int score;
  int depth;
};

void collect_exes_rec(const std::string& base_dir, const std::string& rel_dir,
                      const std::string& game_name, int depth,
                      std::vector<exe_cand>* out) {
  if (depth > 5) return;
  const std::string dir = rel_dir.empty() ? base_dir : (base_dir + "\\" + rel_dir);
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  const std::string g_low = lower_ascii(game_name);
  do {
    const std::string fn = fd.cFileName;
    if (fn == "." || fn == ".." || fn == "__MACOSX") continue;
    const std::string rel = rel_dir.empty() ? fn : (rel_dir + "\\" + fn);
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      const std::string fn_low = lower_ascii(fn);
      if (fn_low == "thirdparty" || fn_low == "extras" || fn_low == "prereqs") {
        continue;
      }
      collect_exes_rec(base_dir, rel, game_name, depth + 1, out);
      continue;
    }
    const size_t dot = fn.rfind('.');
    if (dot == std::string::npos || _stricmp(fn.c_str() + dot, ".exe") != 0) {
      continue;
    }
    if (is_junk_exe(fn)) continue;
    const u64 sz = ((u64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    const std::string stem_low = lower_ascii(fn.substr(0, dot));
    int score = 0;
    if (stem_low == g_low || stem_low == g_low + "-win64-shipping") score += 200;
    if (stem_low.size() > 15 &&
        stem_low.rfind("-win64-shipping") == stem_low.size() - 15) {
      score += 120;
    }
    if (depth == 0) score += 60;
    else if (depth == 1) score += 30;
    out->push_back({rel, sz, score, depth});
  } while (FindNextFileA(h, &fd));
  FindClose(h);
}

// Exe candidates inside a disc image (same scoring as the folder walk).
void collect_iso_exes_rec(trt_iso* iso, const std::string& rel_dir,
                          const std::string& game_name, int depth,
                          std::vector<exe_cand>* out) {
  if (!iso || depth > 5) return;
  struct rec {
    std::vector<std::string> names;
    std::vector<int> dirs;
    std::vector<uint64_t> sizes;
  } r;
  trt_iso_enum(iso, rel_dir.c_str(), [](void* user, const char* name, int is_dir,
                                        uint64_t size) {
    rec* rr = (rec*)user;
    rr->names.push_back(name);
    rr->dirs.push_back(is_dir);
    rr->sizes.push_back(size);
  }, &r);
  const std::string g_low = lower_ascii(game_name);
  for (size_t i = 0; i < r.names.size(); ++i) {
    const std::string& fn = r.names[i];
    const std::string rel = rel_dir.empty() ? fn : (rel_dir + "\\" + fn);
    if (r.dirs[i]) {
      const std::string fn_low = lower_ascii(fn);
      if (fn_low == "thirdparty" || fn_low == "extras" || fn_low == "prereqs" ||
          fn_low == "directx" || fn_low == "redist")
        continue;
      collect_iso_exes_rec(iso, rel, game_name, depth + 1, out);
      continue;
    }
    const size_t dot = fn.rfind('.');
    if (dot == std::string::npos || _stricmp(fn.c_str() + dot, ".exe") != 0)
      continue;
    if (is_junk_exe(fn)) continue;
    const std::string stem_low = lower_ascii(fn.substr(0, dot));
    int score = 0;
    if (stem_low == g_low || stem_low == g_low + "-win64-shipping") score += 200;
    if (stem_low.size() > 15 &&
        stem_low.rfind("-win64-shipping") == stem_low.size() - 15)
      score += 120;
    if (depth == 0) score += 60;
    else if (depth == 1) score += 30;
    out->push_back({rel, r.sizes[i], score, depth});
  }
}

// Static compatibility scan of a game's exe: machine type + import table.
// Seeks to real RVA sections so multi-MB Unreal/Unity PE64s inspect accurately.
void scan_compat_stream(std::istream& f, char* out, unsigned cap) {
  u8 hdr[4096] = {};
  f.read((char*)hdr, sizeof(hdr));
  const size_t got = (size_t)f.gcount();
  if (got < 0x40 || hdr[0] != 'M' || hdr[1] != 'Z') {
    snprintf(out, cap, "compat: not a PE"); return;
  }
  u32 e_lfanew = 0;
  memcpy(&e_lfanew, hdr + 0x3C, 4);
  if (e_lfanew + 24 > got || hdr[e_lfanew] != 'P' || hdr[e_lfanew + 1] != 'E') {
    snprintf(out, cap, "compat: PE signature missing"); return;
  }
  const u16 machine = (u16)(hdr[e_lfanew + 4] | (hdr[e_lfanew + 5] << 8));
  const u16 num_sec = (u16)(hdr[e_lfanew + 6] | (hdr[e_lfanew + 7] << 8));
  const u16 opt_size = (u16)(hdr[e_lfanew + 20] | (hdr[e_lfanew + 21] << 8));
  const u16 opt_magic = (u16)(hdr[e_lfanew + 24] | (hdr[e_lfanew + 25] << 8));
  const char* arch;
  if (machine == 0x8664 && opt_magic == 0x20B) arch = "x64";
  else if (machine == 0x014C) arch = "x86 32-bit (unsupported)";
  else if (machine == 0xAA64) arch = "ARM64 (unsupported)";
  else arch = "unknown arch";

  u32 imp_rva = 0, imp_sz = 0;
  const u32 opt_off = e_lfanew + 24;
  if (opt_magic == 0x20B && opt_off + 128 <= got) {
    memcpy(&imp_rva, hdr + opt_off + 120, 4);
    memcpy(&imp_sz, hdr + opt_off + 124, 4);
  } else if (opt_magic == 0x10B && opt_off + 112 <= got) {
    memcpy(&imp_rva, hdr + opt_off + 104, 4);
    memcpy(&imp_sz, hdr + opt_off + 108, 4);
  }
  (void)imp_sz;

  const u32 sec_off = opt_off + opt_size;
  auto rva_to_file = [&](u32 rva) -> u64 {
    const u32 secs = num_sec < 96 ? num_sec : 96;
    for (u32 s = 0; s < secs; ++s) {
      const u32 so = sec_off + s * 40;
      if (so + 40 > got) break;
      u32 vaddr = 0, vsize = 0, raw = 0, rawsz = 0;
      memcpy(&vsize, hdr + so + 8, 4);
      memcpy(&vaddr, hdr + so + 12, 4);
      memcpy(&rawsz, hdr + so + 16, 4);
      memcpy(&raw, hdr + so + 20, 4);
      const u32 span = vsize > rawsz ? vsize : rawsz;
      if (rva >= vaddr && rva < vaddr + span && (rva - vaddr) < rawsz) {
        return (u64)raw + (u64)(rva - vaddr);
      }
    }
    return 0;
  };

  char mods[256] = "";
  const u64 imp_file = imp_rva ? rva_to_file(imp_rva) : 0;
  if (imp_file > 0) {
    u8 desc[2048] = {};
    f.clear();
    f.seekg((std::streamoff)imp_file, std::ios::beg);
    f.read((char*)desc, sizeof(desc));
    const size_t dgot = (size_t)f.gcount();
    size_t o = 0;
    for (u32 d = 0; d * 20 + 20 <= dgot && o + 40 < sizeof(mods); ++d) {
      u32 name_rva = 0;
      memcpy(&name_rva, desc + d * 20 + 12, 4);
      if (!name_rva) break;
      const u64 nfile = rva_to_file(name_rva);
      if (!nfile) break;
      char nm[64] = {};
      f.clear();
      f.seekg((std::streamoff)nfile, std::ios::beg);
      f.read(nm, sizeof(nm) - 1);
      nm[sizeof(nm) - 1] = 0;
      if (!nm[0]) continue;
      const char* mark = "";
      if (!_strnicmp(nm, "steam_api", 9)) mark = "[steam-auth]";
      else if (!_strnicmp(nm, "d3d12", 5)) mark = "[d3d12 OK]";
      else if (!_strnicmp(nm, "d3d9", 4)) mark = "[d3d9 OK]";
      else if (!_strnicmp(nm, "d3d8", 4)) mark = "[d3d8 OK]";
      else if (!_strnicmp(nm, "d3dx9", 5)) mark = "[d3dx9 OK]";
      else if (!_strnicmp(nm, "dsound", 6)) mark = "[dsound OK]";
      else if (!_strnicmp(nm, "ddraw", 5)) mark = "[ddraw OK]";
      else if (!_strnicmp(nm, "d3d11", 5)) mark = "[d3d11 OK]";
      else if (!_strnicmp(nm, "xaudio", 6)) mark = "[audio OK]";
      else if (!_strnicmp(nm, "openal", 6)) mark = "[openal OK]";
      else if (!_strnicmp(nm, "xinput", 6)) mark = "[pad OK]";
      else if (!_strnicmp(nm, "gameinput", 9)) mark = "[gameinput OK]";
      else if (!_strnicmp(nm, "winhttp", 7)) mark = "[winhttp OK]";
      else if (!_strnicmp(nm, "vulkan-1", 8)) mark = "[VK->D3D OK]";
      else if (!_strnicmp(nm, "opengl32", 8)) mark = "[GL->D3D OK]";
      else if (!_strnicmp(nm, "unityplayer", 11)) mark = "[unity OK]";
      o += (size_t)snprintf(mods + o, sizeof(mods) - o, " %s%s", nm, mark);
    }
  }
  snprintf(out, cap, "compat: %s;%s", arch, mods[0] ? mods : " no imports");
}

void scan_compat(const std::string& path, char* out, unsigned cap) {
  std::ifstream f(path, std::ios::binary);
  if (!f) { snprintf(out, cap, "compat: exe unreadable"); return; }
  scan_compat_stream(f, out, cap);
}

// The same static scan for an exe that lives inside a disc image: read it out
// of the image (bounded) and analyse it in memory.
void scan_compat_iso(trt_iso* iso, const std::string& rel, char* out,
                     unsigned cap) {
  int is_dir = 0;
  uint64_t size = 0;
  if (!iso || trt_iso_stat(iso, rel.c_str(), &is_dir, &size) != 0 || is_dir) {
    snprintf(out, cap, "compat: exe unreadable");
    return;
  }
  const uint64_t cap_bytes = 96ull * 1024 * 1024;
  const uint64_t want = size < cap_bytes ? size : cap_bytes;
  std::string buf((size_t)want, '\0');
  const uint32_t got = trt_iso_read(iso, rel.c_str(), 0, buf.data(), (uint32_t)want);
  buf.resize(got);
  std::istringstream is(buf, std::ios::binary);
  scan_compat_stream(is, out, cap);
}

void scan_games_root(
    const std::string& root, char drive_tag,
    const std::vector<std::pair<std::string, std::string>>& prev_choice) {
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA((root + "\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    const std::string n = fd.cFileName;
    if (n == "." || n == "..") continue;
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
      // A disc image dropped straight into the games folder: the exe list is
      // read out of the image and it mounts as D: on launch.
      const size_t dot = n.rfind('.');
      if (dot == std::string::npos) continue;
      const std::string ext = lower_ascii(n.substr(dot));
      if (ext != ".iso" && ext != ".bin" && ext != ".img") continue;
      const std::string img_path = root + "\\" + n;
      bool dup_img = false;
      for (const auto& existing : g_games)
        if (!_stricmp(existing.image.c_str(), img_path.c_str())) dup_img = true;
      if (dup_img) continue;
      trt_iso* iso = trt_iso_open(img_path.c_str());
      if (!iso) {
        char line[600];
        snprintf(line, sizeof(line),
                 "shelf: %s is not an ISO9660/UDF image (skipped, named)",
                 img_path.c_str());
        longan::log_line(line);
        continue;
      }
      std::vector<exe_cand> icands;
      const std::string stem = n.substr(0, dot);
      collect_iso_exes_rec(iso, "", stem, 0, &icands);
      if (icands.empty()) {
        char line[600];
        snprintf(line, sizeof(line),
                 "shelf: %s mounted but holds no runnable exe", img_path.c_str());
        longan::log_line(line);
        trt_iso_close(iso);
        continue;
      }
      std::sort(icands.begin(), icands.end(),
                [](const exe_cand& a, const exe_cand& b) {
        if (a.score != b.score) return a.score > b.score;
        if (a.size != b.size) return a.size > b.size;
        return a.rel < b.rel;
      });
      game_entry ge;
      ge.name = stem;
      ge.dir = root;
      ge.image = img_path;
      for (const auto& c : icands) ge.exes.push_back(c.rel);
      ge.exe = ge.exes[0];
      char compat[280];
      scan_compat_iso(iso, ge.exe, compat, (unsigned)sizeof(compat));
      ge.compat = compat;
      trt_iso_close(iso);
      g_games.push_back(std::move(ge));
      continue;
    }
    const std::string gdir = root + "\\" + n;
    std::string disp = n;
    bool dup = false;
    for (const auto& existing : g_games) {
      if (!_stricmp(existing.dir.c_str(), gdir.c_str())) {
        dup = true;
        break;
      }
      if (existing.name == disp && drive_tag) {
        disp = n + " [" + std::string(1, drive_tag) + ":]";
      }
    }
    if (dup) continue;
    std::vector<exe_cand> cands;
    collect_exes_rec(gdir, "", n, 0, &cands);
    // A folder that holds a disc image is a game too: the exe list comes from
    // inside the image and the image is mounted as D: at launch.
    std::string image_path;
    if (cands.empty()) {
      char img[1024] = {};
      if (trt_iso_find_image_in_dir(gdir.c_str(), img, sizeof(img))) {
        trt_iso* iso = trt_iso_open(img);
        if (iso) {
          collect_iso_exes_rec(iso, "", n, 0, &cands);
          if (!cands.empty()) image_path = img;
          trt_iso_close(iso);
        }
        if (cands.empty()) {
          char line[600];
          snprintf(line, sizeof(line),
                   "shelf: %s holds a disc image but no runnable exe inside it",
                   img);
          longan::log_line(line);
        }
      }
    }
    if (cands.empty()) continue;
    std::sort(cands.begin(), cands.end(), [](const exe_cand& a, const exe_cand& b) {
      if (a.score != b.score) return a.score > b.score;
      if (a.size != b.size) return a.size > b.size;
      if (a.depth != b.depth) return a.depth < b.depth;
      return a.rel < b.rel;
    });
    game_entry ge;
    ge.name = disp;
    ge.dir = gdir;
    for (const auto& c : cands) ge.exes.push_back(c.rel);
    ge.exe_idx = 0;
    {
      std::ifstream ef(gdir + "\\tiramisu_exe.txt");
      std::string cfg_exe;
      if (ef && std::getline(ef, cfg_exe)) {
        while (!cfg_exe.empty() && ((unsigned char)cfg_exe.back() <= ' '))
          cfg_exe.pop_back();
        for (size_t k = 0; k < ge.exes.size(); ++k) {
          if (!_stricmp(ge.exes[k].c_str(), cfg_exe.c_str())) {
            ge.exe_idx = (int)k;
            break;
          }
        }
      }
      std::ifstream inf(gdir + "\\tiramisu_input.txt");
      std::string im;
      if (inf && std::getline(inf, im)) {
        while (!im.empty() && ((unsigned char)im.back() <= ' ')) im.pop_back();
        ge.input_mode = lower_ascii(im);
      }
    }
    for (const auto& p : prev_choice) {
      if (p.first == (ge.image.empty() ? gdir : ge.image)) {
        for (size_t k = 0; k < ge.exes.size(); ++k) {
          if (ge.exes[k] == p.second) {
            ge.exe_idx = (int)k;
            break;
          }
        }
        break;
      }
    }
    ge.exe = ge.exes[ge.exe_idx];
    ge.image = image_path;
    char compat[280];
    if (!ge.image.empty()) {
      trt_iso* iso = trt_iso_open(ge.image.c_str());
      scan_compat_iso(iso, ge.exe, compat, (unsigned)sizeof(compat));
      trt_iso_close(iso);
    } else {
      scan_compat(gdir + "\\" + ge.exe, compat, (unsigned)sizeof(compat));
    }
    ge.compat = compat;
    g_games.push_back(std::move(ge));
  } while (FindNextFileA(h, &fd));
  FindClose(h);
}

void rescan_games() {
  std::vector<std::pair<std::string, std::string>> prev_choice;
  for (const auto& ge : g_games) {
    prev_choice.push_back({!ge.image.empty() ? ge.image
                            : (ge.dir.empty() ? ge.name : ge.dir),
                           ge.exe});
  }
  g_games.clear();
  scan_games_root(g_games_dir, 0, prev_choice);
  for (char drv = 'D'; drv <= 'Y'; ++drv) {
    const char root_drv[4] = {drv, ':', '\\', 0};
    const UINT dt = GetDriveTypeA(root_drv);
    if (dt == DRIVE_REMOVABLE || dt == DRIVE_FIXED) {
      scan_games_root(std::string(1, drv) + ":\\Games", drv, prev_choice);
      scan_games_root(std::string(1, drv) + ":\\TiramisuGames", drv, prev_choice);
    }
  }
  if (g_selected >= (int)g_games.size()) g_selected = 0;
}

// ---------------- guest launch ----------------
struct guest_run_result {
  int map_rc;
  char detail[256];
};

std::jmp_buf g_exit_jmp;
int g_exit_code = 0;
u32 g_raise_code = 0;
DWORD g_guest_tid = 0;
std::atomic<bool> g_guest_stop_req{false};
std::atomic<u64> g_guest_stop_since{0};

extern "C" {
void trt_bridge_post_quit(int code);
}

void request_guest_stop() {
  if (!g_guest_stop_req.exchange(true)) {
    g_guest_stop_since.store((u64)GetTickCount64());
    trt_msg_post(0x0010 /*WM_CLOSE*/, 0, 0);
    trt_bridge_post_quit(0);
    longan::log_line("host: stop requested -> posting WM_CLOSE/WM_QUIT");
  }
}

void check_guest_stop_trap() {
  if (g_guest_stop_req.load() && g_guest_tid &&
      GetCurrentThreadId() == g_guest_tid) {
    u64 elapsed = (u64)GetTickCount64() - g_guest_stop_since.load();
    if (elapsed >= 1200ull) {
      g_guest_stop_req.store(false);
      longan::log_line("host: guest did not quit cleanly -> unwinding to shelf");
      if (trt_boot_exit_trap) trt_boot_exit_trap(0);
    }
  }
}

void host_exit_trap(int code) {
  if (GetCurrentThreadId() == g_guest_tid) {
    g_exit_code = code;
    std::longjmp(g_exit_jmp, 1);
  }
  ExitThread((DWORD)code);
}

static void describe_fault_pc(uintptr_t pc, char* out, size_t cap) {
  const char* module = "unknown";
  uintptr_t offset = pc;
  char native_name[MAX_PATH]{};
  MEMORY_BASIC_INFORMATION memory{};
  if (VirtualQuery(reinterpret_cast<void*>(pc), &memory, sizeof(memory))) {
    offset = pc - reinterpret_cast<uintptr_t>(memory.AllocationBase);
    const char* guest = trt_images_name(memory.AllocationBase);
    if (guest) {
      module = guest;
    } else {
      if (GetModuleFileNameA(static_cast<HMODULE>(memory.AllocationBase),
                             native_name, sizeof(native_name))) {
        const char* leaf = strrchr(native_name, '\\');
        module = leaf ? leaf + 1 : native_name;
      }
    }
  }

  snprintf(out, cap, "%s+0x%llx (pc=%p)", module,
           static_cast<unsigned long long>(offset), reinterpret_cast<void*>(pc));
}

static int capture_fault_stack(const CONTEXT* initial, DWORD64* frames, int cap) {
  int count = 0;
  CONTEXT ctx = *initial;
  __try {
    while (ctx.Rip && count < cap) {
      frames[count++] = ctx.Rip;
      const DWORD64 previous_rsp = ctx.Rsp;
      DWORD64 base = 0;
      if (const auto fn = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr)) {
        PVOID handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fn, &ctx, &handler_data,
                         &establisher, nullptr);
      } else {
        ctx.Rip = *reinterpret_cast<const DWORD64*>(ctx.Rsp);
        ctx.Rsp += sizeof(DWORD64);
      }
      if (ctx.Rsp <= previous_rsp) break;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  return count;
}

static int guest_seh_filter(EXCEPTION_POINTERS* ep) {
  if (trt_dispatch_win32_exception(ep) == -1) return EXCEPTION_CONTINUE_EXECUTION;
  const EXCEPTION_RECORD& record = *ep->ExceptionRecord;
  char where[192];
  describe_fault_pc(reinterpret_cast<uintptr_t>(record.ExceptionAddress),
                    where, sizeof(where));
  char line[320];
  if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
      record.NumberParameters >= 2) {
    const ULONG_PTR kind = record.ExceptionInformation[0];
    snprintf(line, sizeof(line),
             "guest fault: 0x%08lx at %s, %s address %p",
             (unsigned long)record.ExceptionCode, where,
             kind == 0 ? "read" : kind == 1 ? "write" : "execute",
             reinterpret_cast<void*>(record.ExceptionInformation[1]));
  } else {
    snprintf(line, sizeof(line), "guest fault: 0x%08lx at %s",
             (unsigned long)record.ExceptionCode, where);
  }
  longan::log_line(line);
  DWORD64 frames[32];
  const int count = capture_fault_stack(ep->ContextRecord, frames, 32);
  for (int i = 1; i < count; ++i) {
    describe_fault_pc(static_cast<uintptr_t>(frames[i]), where, sizeof(where));
    snprintf(line, sizeof(line), "guest fault caller %d: %s", i, where);
    longan::log_line(line);
  }
  return EXCEPTION_EXECUTE_HANDLER;
}

// Pure-POD SEH wrapper: allows C++ try/catch inside guest PE images to unwind
// natively via Windows .pdata while still catching truly unhandled faults.
int call_guest_seh(trt_image* img, u64* out_rc, DWORD* out_ex) {
  __try {
    *out_rc = trt_call_guest_img(
        (void*)(img->base + img->entry_rva), img,
        (u64)(uintptr_t)img->base, 0, 0, 0);
    return 0;
  } __except (guest_seh_filter(GetExceptionInformation())) {
    *out_ex = GetExceptionCode();
    return 1;
  }
}

void run_guest_trapped(trt_image* img, char* detail, size_t cap) {
  g_exit_code = 0;
  g_raise_code = 0;
  g_guest_tid = GetCurrentThreadId();
  trt_boot_exit_trap = &host_exit_trap;
  trt_boot_raise_trap = nullptr;  // let caught C++ EH unwind via OS .pdata
  const int trapped = setjmp(g_exit_jmp);
  if (trapped == 0) {
    u64 rc = 0;
    DWORD ex_code = 0;
    if (call_guest_seh(img, &rc, &ex_code) == 0) {
      snprintf(detail, cap, "guest returned %llu", (unsigned long long)rc);
    } else {
      snprintf(detail, cap, "guest unhandled exception 0x%08lx",
               (unsigned long)ex_code);
    }
  } else if (trapped == 1) {
    snprintf(detail, cap, "guest ExitProcess(%d)", g_exit_code);
  } else {
    snprintf(detail, cap, "guest raised exception 0x%08x", g_raise_code);
  }
  trt_boot_exit_trap = nullptr;
  trt_boot_raise_trap = nullptr;
  g_guest_tid = 0;
}

guest_run_result run_game(const std::string& name, const std::string& exe,
                          const std::string& game_dir,
                          const std::string& image = std::string()) {
  guest_run_result r{};
  char line[256];

  trt_images_reset();
  trt_backend be{&uwp_alloc,     &uwp_protect,        &uwp_release,
                 &uwp_log,       &uwp_register_unwind};
  trt_set_vmem_hooks(&uwp_reserve, &uwp_commit, &uwp_decommit);
  trt_bridge_boot(&be);
  trt_bridge_register_kernel32();
  trt_bridge_register_nvapi();  // nvapi/nvml/atiadl honest not-supported stubs
  trt_bridge_register_user32();
  // PE resources: FindResource/LoadResource/LockResource/LoadStringW walk
  // the game's own .rsrc directory (kernel32 surface, bridge_res.cpp)
  trt_bridge_register_res();
  // BUILTIN graphics/media/GDI/D3D11/DXGI bridges remain registered from
  // trt_bridge_boot; only bind ws2_32 and bcrypt to real SYSTEM UWP DLLs.
  trt_module_register("ws2_32.dll", TRT_ROUTE_SYSTEM, nullptr, 0);
  trt_module_register("bcrypt.dll", TRT_ROUTE_SYSTEM, nullptr, 0);
  trt_set_system_loader(&system_bind);
  trt_set_pump(&host_pump);  // guests that pump keep CoreWindow/XInput alive
  longan::swapchain_bind_d3d9();

  const std::string drive_c = g_local_state + "\\drive_c";
  const char* c_dirs[] = {
      "",
      "\\Temp",
      "\\Users",
      "\\Users\\Player",
      "\\Users\\Player\\AppData",
      "\\Users\\Player\\AppData\\Local",
      "\\Users\\Player\\AppData\\LocalLow",
      "\\Users\\Player\\AppData\\Roaming",
      "\\Users\\Player\\Documents",
      "\\Users\\Player\\Documents\\My Games",
      "\\Users\\Player\\Saved Games",
      "\\Users\\Player\\Desktop",
      "\\Users\\Player\\Pictures",
      "\\Users\\Player\\Music",
      "\\Users\\Player\\Videos",
      "\\ProgramData",
      "\\Windows",
      "\\Windows\\System32",
      "\\Windows\\Temp",
  };
  for (const char* sub : c_dirs)
    trt_vfs_mkdir_p((drive_c + sub).c_str());
  if (_stricmp(name.c_str(), "raccoin") == 0) {
    // The game's offline Steam profile is the default guest account.
    const auto save = drive_c +
        "\\Users\\Player\\AppData\\LocalLow\\DoraccoonGames\\RACCOIN\\SaveData"
        "\\76561197960265729";
    if (trt_vfs_mkdir_p(save.c_str()) != 0)
      longan::log_line("profile: RACCOIN save directory could not be created");
  }
  trt_vfs_mount("c:", drive_c.c_str());
  trt_vfs_mount("z:", game_dir.c_str());
  // Per-game profile overrides (tiramisu.profile.ini next to the exe).
  // Applied on top of global settings; missing file = no-op (returns 0).
  {
    const int prof = longan::settings_load_profile(game_dir);
    char pline[128];
    snprintf(pline, sizeof(pline),
             "profile: %d keys applied from %s\\tiramisu.profile.ini",
             prof, game_dir.c_str());
    longan::log_line(pline);
  }
  // A game that ships on a disc image: mount it as D: (read-only, lazily
  // extracted) and run the exe out of the image, so every one of its path
  // requests - assets, music, movies - is answered by the disc itself.
  std::string guest_exe = "Z:\\" + exe;
  std::string guest_cwd_root = "Z:\\";
  bool image_mounted = false;
  if (!image.empty()) {
    const std::string cache_root = g_local_state + "\\iso-cache";
    trt_vfs_set_iso_cache_root(cache_root.c_str());
    if (trt_vfs_mount_image("d:", image.c_str())) {
      image_mounted = true;
      guest_exe = "D:\\" + exe;
      guest_cwd_root = "D:\\";
      char iline[600];
      snprintf(iline, sizeof(iline), "trt: disc image mounted as D: %s",
               image.c_str());
      longan::log_line(iline);
    } else {
      longan::log_line("trt: disc image mount FAILED (not an ISO9660/UDF "
                       "image) - launching from the folder instead");
    }
  }
  trt_bridge_set_exe_path(guest_exe.c_str());
  {
    const size_t rel_sl = exe.rfind('\\');
    const std::string guest_cwd =
        (rel_sl == std::string::npos) ? guest_cwd_root
                                      : (guest_cwd_root + exe.substr(0, rel_sl));
    trt_bridge_set_cwd(guest_cwd.c_str());
  }
  {
    std::ifstream af(game_dir + "\\tiramisu_args.txt");
    if (af) {
      std::string args, ln;
      while (std::getline(af, ln)) {
        if (!ln.empty() && ln.back() == '\r') ln.pop_back();
        if (ln.empty() || ln[0] == '#') continue;
        if (!args.empty()) args += ' ';
        args += ln;
      }
      trt_bridge_set_cmd_args(args.c_str());
    } else {
      trt_bridge_set_cmd_args("");
    }
    u32 app_id = 0;
    std::ifstream sf(game_dir + "\\steam_appid.txt");
    if (sf) sf >> app_id;
    trt_steam_set_appid(app_id);
  }
  g_guest_stop_req.store(false);
  // the registry hive lives on the guest's C: drive (next to its saves).
  // Windows wchar_t is UTF-16, which is what the bridge expects.
  static_assert(sizeof(wchar_t) == 2, "wchar_t must be UTF-16");
  trt_registry_set_hive_path((const u16*)L"C:\\registry.hiv");

  trt_resolver rs;
  trt_resolver_default(&rs);
  std::string exe_path = game_dir + "\\" + exe;
  if (image_mounted) {
    // The loader reads the PE from the host, so the exe and the DLLs beside it
    // are materialized once from the image into the cache. Every other file the
    // game asks for is extracted on demand through D:.
    const size_t exe_dirs = exe.rfind('\\');
    const std::string rel_dir =
        (exe_dirs == std::string::npos) ? std::string() : exe.substr(0, exe_dirs);
    const std::wstring wdir = u82ws("D:\\" + rel_dir);
    char hdir[1200] = {};
    int ddir = 0;
    if (trt_vfs_image_materialize((const u16*)wdir.c_str(), hdir, sizeof(hdir),
                                  &ddir) == 0 &&
        ddir) {
      struct trt_iso* iso =
          trt_vfs_image_for_guest_utf16((const u16*)wdir.c_str(), nullptr, 0);
      if (iso) trt_iso_extract_dir(iso, rel_dir.c_str(), hdir);
      char hexe[1400] = {};
      const std::wstring wexe = u82ws(guest_exe);
      if (trt_vfs_image_materialize((const u16*)wexe.c_str(), hexe,
                                    sizeof(hexe), nullptr) == 0)
        exe_path = hexe;
    }
  }
  const size_t exe_sl = exe_path.rfind('\\');
  const std::string exe_dir =
      (exe_sl == std::string::npos) ? game_dir : exe_path.substr(0, exe_sl);
  // the runtime knows where the game's own DLLs live, and resolves imports
  // through the guest-image registry (a game's own DLLs beat the system's)
  trt_runtime_init(&be, exe_dir.c_str());
  trt_resolver* runtime_rs = trt_runtime_resolver();

  std::ifstream f(exe_path, std::ios::binary);
  if (!f) {
    snprintf(r.detail, sizeof(r.detail), "cannot open %s", exe_path.c_str());
    return r;
  }
  std::vector<u8> buf((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());

  snprintf(line, sizeof(line), "trt: launching %s (%zu bytes)", exe_path.c_str(),
           buf.size());
  longan::log_line(line);

  trt_image* img = nullptr;
  r.map_rc = trt_map_pe(&be, runtime_rs, buf.data(), buf.size(), &img);
  if (r.map_rc != TRT_OK) {
    strncpy(r.detail, trt_errstr(r.map_rc), sizeof(r.detail) - 1);
    return r;
  }
  if (trt_seal_image(&be, img) != TRT_OK) {
    strncpy(r.detail, "seal failed", sizeof(r.detail) - 1);
    return r;
  }
  trt_images_register_main(img, exe.c_str());
  for (int i = 0; i < img->tls_callback_count; ++i) {
    trt_call_guest_img((void*)(img->base + img->tls_callback_rvas[i]), img,
                       (u64)(uintptr_t)img->base, 1, 0, 0);
  }
  run_guest_trapped(img, r.detail, sizeof(r.detail));
  return r;
}

// ---------------- Steam QR login (own account) ----------------
std::wstring uri_escape(const std::wstring& s) {
  try {
    return std::wstring(
        winrt::Windows::Foundation::Uri::EscapeComponent(s).c_str());
  } catch (...) {
    return s;
  }
}

void steam_begin() {
  if (g_steam_busy.exchange(true)) return;
  g_steam_status = "steam: requesting QR…";
  std::thread([] {
    try {
      winrt::init_apartment(winrt::apartment_type::multi_threaded);
      using namespace winrt::Windows::Web::Http;
      using namespace winrt::Windows::Data::Json;
      HttpClient http;
      // form body assembled by hand: input_protobuf_encoded=false + escaped
      // input JSON (the WebAPI's JSON mode)
      auto post_form = [&http](const wchar_t* endpoint,
                               const std::wstring& input_json)
          -> winrt::Windows::Foundation::IAsyncOperationWithProgress<
              HttpResponseMessage, HttpProgress> {
        const std::wstring body =
            L"input_protobuf_encoded=false&input=" + uri_escape(input_json);
        HttpStringContent content(body,
                                  Windows::Storage::Streams::UnicodeEncoding::Utf8,
                                  L"application/x-www-form-urlencoded");
        return http.PostAsync(
            winrt::Windows::Foundation::Uri(endpoint), content);
      };
      const std::wstring begin_json =
          L"{\"client_id\":\"2654200164598180\",\"device_id\":"
          L"\"tiramisu-xbox\",\"refresh_token\":\"\"}";
      auto resp = post_form(
          L"https://api.steampowered.com/IAuthenticationService/v1/"
          L"BeginAuthSessionViaQR",
          begin_json)
                      .get();
      const std::string text = ws2u8(resp.Content().ReadAsStringAsync().get());
      auto j = JsonObject::Parse(winrt::to_hstring(text));
      auto r2 = j.GetNamedObject(L"response");
      g_steam_qr = str2u8(ws2u8(r2.GetNamedString(L"challenge_url")));
      const std::string client_id =
          str2u8(ws2u8(r2.GetNamedString(L"client_id")));
      const std::string request_id =
          str2u8(ws2u8(r2.GetNamedString(L"request_id")));
      g_steam_status = "steam: scan the QR with the Steam app";
      for (int i = 0; i < 36 && !g_shutdown; ++i) {
        Sleep(5000);
        auto pr =
            post_form(L"https://api.steampowered.com/IAuthenticationService/"
                      L"v1/PollAuthSessionStatus",
                      L"{\"client_id\":\"" + u82ws(client_id) +
                          L"\",\"request_id\":\"" + u82ws(request_id) +
                          L"\"}")
                .get();
        const std::string pt = ws2u8(pr.Content().ReadAsStringAsync().get());
        auto pj = JsonObject::Parse(winrt::to_hstring(pt));
        auto pr2 = pj.GetNamedObject(L"response");
        if (pr2.HasKey(L"refresh_token")) {
          const std::string token =
              str2u8(ws2u8(pr2.GetNamedString(L"refresh_token")));
          FILE* tf = fopen((g_local_state + "\\steam.token").c_str(), "wb");
          if (tf) {
            fwrite(token.data(), 1, token.size(), tf);
            fclose(tf);
          }
          g_steam_status =
              "steam: LINKED — depot DL is M6; play via file transfer now";
          g_steam_qr.clear();
          break;
        }
      }
      if (g_steam_qr.size()) {
        g_steam_qr.clear();
        g_steam_status = "steam: QR expired — press Y to retry";
      }
    } catch (hresult_error const& e) {
      g_steam_status = "steam error: " + ws2u8(e.message());
    } catch (...) {
      g_steam_status = "steam error: network/parse (endpoint may need protobuf)";
    }
    g_steam_busy = false;
  }).detach();
}

// ---------------- dashboard: settings rows + screen nav ----------------
// Rows mirror the live host_settings; edited in place, applied+persisted on
// change. Value buffers are the ui_setting value[] arrays themselves (UTF-16,
// what the renderer draws).
const wchar_t* k_input_choices[4] = {L"auto", L"controller", L"kbm",
                                     L"pad2kbm"};
const wchar_t* k_cap_choices[4] = {L"0", L"30", L"60", L"120"};

void sync_settings_rows() {
  const auto& s = longan::settings_get();
  auto set_bool = [](longan::ui_setting& r, bool v) {
    r.is_bool = true; r.is_choice = false; r.is_int = false;
    r.value[0] = v ? L'1' : L'0'; r.value[1] = 0;
  };
  auto set_cap = [](longan::ui_setting& r, int v) {
    r.is_bool = false; r.is_choice = true; r.is_int = false;
    const wchar_t* best = k_cap_choices[0];
    for (int k = 0; k < 4; ++k)
      if (_wtoi(k_cap_choices[k]) == v) best = k_cap_choices[k];
    wcscpy(r.value, best);
  };
  int i = 0;
  // 0 default input
  wcscpy(g_set_rows[i].label, L"Default input mode");
  g_set_rows[i].is_bool = false; g_set_rows[i].is_choice = true;
  g_set_rows[i].is_int = false;
  int ci = 0;
  for (int k = 0; k < 4; ++k)
    if (wcscmp(k_input_choices[k], u82ws(s.default_input).c_str()) == 0) ci = k;
  wcscpy(g_set_rows[i].value, k_input_choices[ci]);
  g_set_rows[i].value[63] = 0;
  ++i;
  // 1 mouse speed
  wcscpy(g_set_rows[i].label, L"Pad-as-mouse speed (1-100)");
  g_set_rows[i].is_bool = false; g_set_rows[i].is_choice = false;
  g_set_rows[i].is_int = true;
  swprintf(g_set_rows[i].value, 64, L"%d", s.mouse_speed);
  ++i;
  // 2 wheel speed
  wcscpy(g_set_rows[i].label, L"Pad wheel speed (1-100)");
  g_set_rows[i].is_int = true;
  swprintf(g_set_rows[i].value, 64, L"%d", s.wheel_speed);
  ++i;
  // 3 vibration
  wcscpy(g_set_rows[i].label, L"Pad vibration (guest rumble)");
  set_bool(g_set_rows[i], s.pad_vibration);
  ++i;
  // 4 OS personality
  wcscpy(g_set_rows[i].label, L"OS reported to games (major.minor.build)");
  g_set_rows[i].is_bool = false; g_set_rows[i].is_choice = false;
  g_set_rows[i].is_int = false;
  swprintf(g_set_rows[i].value, 64, L"%d.%d.%d", s.os_major, s.os_minor,
           s.os_build);
  ++i;
  // 5 Win11 personality
  wcscpy(g_set_rows[i].label, L"Win11 personality (build 22000+)");
  set_bool(g_set_rows[i], s.win11_personality);
  ++i;
  // 6 write watch
  wcscpy(g_set_rows[i].label, L"Write-watch GC (GetWriteWatch)");
  set_bool(g_set_rows[i], s.write_watch);
  ++i;
  // 7 steam auth
  wcscpy(g_set_rows[i].label, L"Steam QR link (own account)");
  set_bool(g_set_rows[i], s.steam_auth);
  ++i;
  // 8 show QR
  wcscpy(g_set_rows[i].label, L"Show QR / LAN link on TV");
  set_bool(g_set_rows[i], s.show_qr);
  ++i;
  // 9 FPS overlay
  wcscpy(g_set_rows[i].label, L"Show FPS counter");
  set_bool(g_set_rows[i], s.show_fps);
  ++i;
  // 10 sysinfo overlay
  wcscpy(g_set_rows[i].label, L"Show system specs overlay");
  set_bool(g_set_rows[i], s.show_sysinfo);
  ++i;
  // 11 quick menu
  wcscpy(g_set_rows[i].label, L"Back+Start quick menu");
  set_bool(g_set_rows[i], s.quick_menu_enabled);
  ++i;
  // 12 vsync
  wcscpy(g_set_rows[i].label, L"VSync");
  set_bool(g_set_rows[i], s.vsync);
  ++i;
  // 13 frame rate cap
  wcscpy(g_set_rows[i].label, L"Frame rate cap (0=off)");
  set_cap(g_set_rows[i], s.frame_rate_cap);
  ++i;
  // 14 memory budget
  wcscpy(g_set_rows[i].label, L"Memory budget MB (guest heap)");
  g_set_rows[i].is_int = true;
  swprintf(g_set_rows[i].value, 64, L"%d", s.memory_budget_mb);
  ++i;
  // 15 input deadzone
  wcscpy(g_set_rows[i].label, L"Controller deadzone (0-20000)");
  g_set_rows[i].is_int = true;
  swprintf(g_set_rows[i].value, 64, L"%d", s.input_deadzone);
  ++i;
  // 16 audio buffer
  wcscpy(g_set_rows[i].label, L"Audio buffer ms");
  g_set_rows[i].is_int = true;
  swprintf(g_set_rows[i].value, 64, L"%d", s.audio_buffer_ms);
  ++i;
  // 17 shader cache
  wcscpy(g_set_rows[i].label, L"Shader compile cache");
  set_bool(g_set_rows[i], s.shader_cache);
  ++i;
  g_set_count = i;
}

// Apply the current row buffers back into host_settings and persist.
void apply_settings_rows() {
  longan::host_settings s = longan::settings_get();
  for (int i = 0; i < g_set_count; ++i) {
    const longan::ui_setting& r = g_set_rows[i];
    if (i == 0) {
      for (int k = 0; k < 4; ++k)
        if (wcscmp(r.value, k_input_choices[k]) == 0) {
          char buf[16];
          size_t n = 0;
          for (const wchar_t* p = k_input_choices[k]; *p && n < 15; ++p)
            buf[n++] = (char)*p;
          buf[n] = 0;
          s.default_input = buf;
        }
    } else if (i == 1) s.mouse_speed = _wtoi(r.value);
    else if (i == 2) s.wheel_speed = _wtoi(r.value);
    else if (i == 3) s.pad_vibration = (wcscmp(r.value, L"1") == 0);
    else if (i == 4) swscanf(r.value, L"%d.%d.%d", &s.os_major, &s.os_minor,
                             &s.os_build);
    else if (i == 5) s.win11_personality = (wcscmp(r.value, L"1") == 0);
    else if (i == 6) s.write_watch = (wcscmp(r.value, L"1") == 0);
    else if (i == 7) s.steam_auth = (wcscmp(r.value, L"1") == 0);
    else if (i == 8) s.show_qr = (wcscmp(r.value, L"1") == 0);
    else if (i == 9) s.show_fps = (wcscmp(r.value, L"1") == 0);
    else if (i == 10) s.show_sysinfo = (wcscmp(r.value, L"1") == 0);
    else if (i == 11) s.quick_menu_enabled = (wcscmp(r.value, L"1") == 0);
    else if (i == 12) s.vsync = (wcscmp(r.value, L"1") == 0);
    else if (i == 13) s.frame_rate_cap = _wtoi(r.value);
    else if (i == 14) s.memory_budget_mb = _wtoi(r.value);
    else if (i == 15) s.input_deadzone = _wtoi(r.value);
    else if (i == 16) s.audio_buffer_ms = _wtoi(r.value);
    else if (i == 17) s.shader_cache = (wcscmp(r.value, L"1") == 0);
  }
  longan::settings_store(s, g_local_state);
  char line[128];
  longan::log_line("settings: saved from the TV dashboard");
  (void)line;
}

// Cycle/edit the selected row: dir = -1/+1 for LR cycles and int edit,
// toggle for bools. Returns true when something changed.
bool edit_setting_row(int dir) {
  if (g_set_selected < 0 || g_set_selected >= g_set_count) return false;
  longan::ui_setting& r = g_set_rows[g_set_selected];
  if (r.is_bool && dir == 1) {
    r.value[0] = (wcscmp(r.value, L"1") == 0) ? L'0' : L'1';
    r.value[1] = 0;
    return true;
  }
  if (r.is_choice) {
    // row-specific choice lists: frame cap (13) cycles 0/30/60/120,
    // everything else is the input-mode list.
    const wchar_t** list = (g_set_selected == 13) ? k_cap_choices
                                                  : k_input_choices;
    int cur = 0;
    for (int k = 0; k < 4; ++k)
      if (wcscmp(r.value, list[k]) == 0) cur = k;
    cur = (cur + dir + 4) % 4;
    wcscpy(r.value, list[cur]);
    return true;
  }
  if (r.is_int) {
    // per-row step and range: mouse/wheel speeds 1..100, memory budget
    // 128..8192 MB, deadzone 0..20000, audio buffer 10..500 ms
    int step = 2, lo = 1, hi = 100;
    if (g_set_selected == 2) { step = 5; }
    else if (g_set_selected == 14) { step = 128; lo = 128; hi = 8192; }
    else if (g_set_selected == 15) { step = 500; lo = 0; hi = 20000; }
    else if (g_set_selected == 16) { step = 10; lo = 10; hi = 500; }
    int v = _wtoi(r.value) + dir * step;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    swprintf(r.value, 64, L"%d", v);
    return true;
  }
  return false;  // text row: keyboard edits through CoreWindow
}

// ---------------- the app ----------------
// BUILTIN provider for tiramisu_host.dll (guest-importable host surface).
// BOTH exports must be here: guest_hello imports LogU32 AND QueryBuild —
// a missing one fails the whole import bind (rc=IMPORT on the console).
static void TRT_GUEST_ABI host_builtin_log_u32(unsigned tag, unsigned value) {
  char line[64];
  snprintf(line, sizeof(line), "guest marker: tag=%u value=%u", tag, value);
  longan::log_line(line);
}
static unsigned TRT_GUEST_ABI host_builtin_query_build(void) {
  return 0x00020100u;  // runtime build 0.2.1
}

struct tiramisu_host
    : implements<tiramisu_host, IFrameworkViewSource, IFrameworkView> {
  CoreWindow m_window{nullptr};
  unsigned m_frames = 0;
  unsigned m_rescan_at = 0;

  IFrameworkView CreateView() { return *this; }
  void Initialize(CoreApplicationView const&) {}
  void Load(hstring const&) {}
  void Uninitialize() {
    g_shutdown.store(true);
    if (g_guest_thread.joinable()) {
      if (g_guest_running.load()) g_guest_thread.detach();
      else g_guest_thread.join();
    }
    longan::audio_shutdown();
  }

  void SetWindow(CoreWindow const& window) {
    m_window = window;
    window.PointerCursor(CoreCursor(CoreCursorType::Arrow, 0));
    const auto sc = longan::swapchain_create(window);
    char line[256];
    snprintf(line, sizeof(line),
             "probe: d3d11 hr=0x%08lx fl=%u swapchain hr=0x%08lx %ux%u flip",
             (unsigned long)sc.device_hr, sc.feature_level,
             (unsigned long)sc.sc_hr, sc.width, sc.height);
    longan::log_line(line);

    ID2D1Factory1* d2df = nullptr;
    D2D1_FACTORY_OPTIONS fo{};
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                      __uuidof(ID2D1Factory1), &fo, (void**)&d2df);
    IDWriteFactory* dwf = nullptr;
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        (IUnknown**)&dwf);
    void* d2d_device_v = nullptr;
    ID3D11Device1* d3d = (ID3D11Device1*)longan::swapchain_d3d();
    if (d3d) {
      IDXGIDevice* dxgidev = nullptr;
      d3d->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgidev);
      if (dxgidev) {
        ID2D1Device* d2ddev = nullptr;
        d2df->CreateDevice(dxgidev, &d2ddev);
        d2d_device_v = d2ddev;
        dxgidev->Release();
      }
    }
    longan::ui_init(sc.width, sc.height, d2df, dwf, d2d_device_v,
                    longan::swapchain_dxgi());
    longan::swapchain_bind_d3d9();
    trt_input_set_cursor_pos(static_cast<int>(sc.width / 2),
                             static_cast<int>(sc.height / 2));

    m_kd = window.KeyDown([](CoreWindow const&, KeyEventArgs const& a) {
      if (g_native_input.keyboard_active()) return;
      static bool logged = false;
      if (!logged) {
        logged = true;
        longan::log_line("input: physical keyboard event received");
      }
      const auto s = a.KeyStatus();
      const u16 vk = disambiguate_modifier_vk((u16)a.VirtualKey(),
                                              (u16)s.ScanCode,
                                              s.IsExtendedKey);
      if (handle_host_cursor_key(vk, (u16)s.ScanCode, s.IsExtendedKey, true)) return;
      trt_input_key_event2(vk, true, (u16)s.ScanCode,
                           s.IsExtendedKey ? 1 : 0, s.WasKeyDown ? 1 : 0);
    });
    m_ku = window.KeyUp([](CoreWindow const&, KeyEventArgs const& a) {
      if (g_native_input.keyboard_active()) return;
      const auto s = a.KeyStatus();
      const u16 vk = disambiguate_modifier_vk((u16)a.VirtualKey(),
                                              (u16)s.ScanCode,
                                              s.IsExtendedKey);
      if (handle_host_cursor_key(vk, (u16)s.ScanCode, s.IsExtendedKey, false)) return;
      trt_input_key_event2(vk, false, (u16)s.ScanCode,
                           s.IsExtendedKey ? 1 : 0, 0);
    });
    m_cr = window.CharacterReceived(
        [](CoreWindow const&, CharacterReceivedEventArgs const& a) {
          if (g_native_input.keyboard_active() || g_interceptor_kbd_active.load()) return;
          trt_input_char_event((u32)a.KeyCode());
        });
    m_pe = window.PointerEntered([](CoreWindow const&, PointerEventArgs const& a) {
      static bool logged = false;
      if (!logged) {
        logged = true;
        char line[96];
        std::snprintf(line, sizeof(line), "input: CoreWindow PointerEntered type=%u",
                      static_cast<unsigned>(a.CurrentPoint().PointerDevice().PointerDeviceType()));
        longan::log_line(line);
      }
    });
    m_aka = window.Dispatcher().AcceleratorKeyActivated(
        [](CoreDispatcher const&, AcceleratorKeyEventArgs const& a) {
          const auto t = a.EventType();
          const u16 vk = static_cast<u16>(a.VirtualKey());
          if (vk >= 195 && vk <= 218) return;
          static bool logged = false;
          if (!logged) {
            logged = true;
            char line[96];
            std::snprintf(line, sizeof(line),
                          "input: AcceleratorKeyActivated type=%u vk=0x%x",
                          static_cast<unsigned>(t), vk);
            longan::log_line(line);
          }
        });
    m_pp = window.PointerPressed([](CoreWindow const& sender,
                                    PointerEventArgs const& a) {
      if (g_native_input.mouse_active()) return;
      const auto point = a.CurrentPoint();
      feed_core_pointer(sender, point.Position());
      const int btn = host_mouse_btn_for_press(point.Properties().PointerUpdateKind());
      if (btn >= 0) {
        static bool logged = false;
        if (!logged) {
          logged = true;
          longan::log_line("input: physical mouse button received");
        }
        trt_input_mouse_button(btn, true);
      }
    });
    m_pr = window.PointerReleased([this](CoreWindow const& sender,
                                         PointerEventArgs const& a) {
      if (g_native_input.mouse_active()) return;
      const auto point = a.CurrentPoint();
      feed_core_pointer(sender, point.Position());
      const int btn = host_mouse_btn_for_release(point.Properties().PointerUpdateKind());
      if (btn >= 0) trt_input_mouse_button(btn, false);
      if (btn == 0) launcher_click();
    });
    m_pm = window.PointerMoved([](CoreWindow const& sender,
                                  PointerEventArgs const& a) {
      if (g_native_input.mouse_active()) return;
      static bool logged = false;
      if (!logged) {
        logged = true;
        longan::log_line("input: physical pointer motion received");
      }
      const auto point = a.CurrentPoint();
      feed_core_pointer(sender, point.Position());
      const auto properties = point.Properties();
      const bool held[5] = {properties.IsLeftButtonPressed(),
                            properties.IsRightButtonPressed(),
                            properties.IsMiddleButtonPressed(),
                            properties.IsXButton1Pressed(),
                            properties.IsXButton2Pressed()};
      for (unsigned button = 0; button < 5; ++button) {
        if (trt_mouse_button(button) != held[button])
          trt_input_mouse_button(button, held[button]);
      }
      static bool type_logged = false;
      if (!type_logged) {
        type_logged = true;
        const auto type = point.PointerDevice().PointerDeviceType();
        char line[96];
        std::snprintf(line, sizeof(line),
                      "input: physical pointer motion type=%u",
                      static_cast<unsigned>(type));
        longan::log_line(line);
      }
    });
    m_pw = window.PointerWheelChanged([](CoreWindow const& sender,
                                         PointerEventArgs const& a) {
      if (g_native_input.mouse_active()) return;
      const auto point = a.CurrentPoint();
      feed_core_pointer(sender, point.Position());
      trt_input_wheel(static_cast<i32>(point.Properties().MouseWheelDelta()));
    });
    m_input_enabled = window.InputEnabled([](CoreWindow const&, InputEnabledEventArgs const& a) {
      longan::log_line(a.InputEnabled() ? "input: CoreWindow input enabled"
                                       : "input: CoreWindow input disabled");
    });
    m_focus = window.Activated([](CoreWindow const&, WindowActivatedEventArgs const& a) {
      const bool focused = a.WindowActivationState() != CoreWindowActivationState::Deactivated;
      longan::log_line(focused ? "input: CoreWindow activated" : "input: CoreWindow deactivated");
      g_input_focused = focused;
      if (!focused) {
        trt_input_release_all();
        g_native_input.suspend();
      }
      if (g_guest_running.load()) {
        trt_msg_post(0x001Cu /*WM_ACTIVATEAPP*/, focused ? 1 : 0, 0);
        trt_msg_post(0x0006u /*WM_ACTIVATE*/, focused ? 1 : 0, 0);
        trt_msg_post(focused ? 0x0007u /*WM_SETFOCUS*/ : 0x0008u /*WM_KILLFOCUS*/, 0, 0);
      }
    });

  }

  void apply_web_controls() {
    const auto events = longan::webxfer_control_events();
    for (const auto& event : events) {
      const bool down = event.second != 0;
      switch (event.kind) {
        case 'a': {
          int w, h, x, y;
          trt_display_size(&w, &h);
          trt_mouse_pos(&x, &y);
          trt_input_mouse_delta_raw(event.first * (w - 1) / 1000 - x,
                                    event.second * (h - 1) / 1000 - y);
          break;
        }
        case 'm':
          trt_input_mouse_delta_raw(event.first, event.second);
          break;
        case 'b':
          m_web_buttons[event.first] = down;
          trt_input_mouse_button(event.first, down);
          if (event.first == 0 && !down) launcher_click();
          break;
        case 'k': {
          const auto vk = static_cast<u16>(event.first);
          const u32 scan = trt_MapVirtualKeyW(vk, 4 /*MAPVK_VK_TO_VSC_EX*/);
          const bool repeat = down && m_web_keys[vk];
          m_web_keys[vk] = down;
          if (handle_host_cursor_key(vk, static_cast<u16>(scan & 0xff),
                                     (scan >> 8) != 0, down)) {
            break;
          }
          trt_input_key_event2(vk, down, static_cast<u16>(scan & 0xff),
                               (scan >> 8) != 0, repeat);
          break;
        }
        case 'w':
          trt_input_wheel(event.first);
          break;
        case 't':
          trt_input_char_event(static_cast<u32>(event.first));
          break;
        case 'r':
          g_f8_down.store(false);
          for (auto& k : g_km_keys) k.store(false);
          for (unsigned i = 0; i < 5; ++i) {
            if (m_web_buttons[i]) trt_input_mouse_button(i, false);
            m_web_buttons[i] = false;
          }
          for (u16 vk = 8; vk < 255; ++vk) {
            if (!m_web_keys[vk]) continue;
            const u32 scan = trt_MapVirtualKeyW(vk, 4);
            trt_input_key_event2(vk, false, static_cast<u16>(scan & 0xff),
                                 (scan >> 8) != 0, false);
            m_web_keys[vk] = false;
          }
          break;
      }
    }
  }

  void launcher_click() {
    if (g_guest_running.load()) return;
    int x, y;
    trt_mouse_pos(&x, &y);
    const auto hit = longan::ui_hit_test(x, y);
    switch (hit.type) {
      case longan::ui_click_type::open_home:
        if (g_setup_active && g_setup_done) {
          g_setup_active = false;
          longan::ui_dismiss_first_run();
        }
        break;
      case longan::ui_click_type::tab:
        if (g_setup_active) break;
        g_screen = static_cast<longan::ui_screen>(hit.index);
        longan::ui_set_screen(g_screen);
        if (g_screen == longan::UI_SETTINGS) {
          sync_settings_rows();
          longan::ui_set_settings(g_set_rows, g_set_count, g_set_selected);
        }
        break;
      case longan::ui_click_type::select_game:
        if (hit.index >= 0 && hit.index < static_cast<int>(g_games.size()))
          g_selected = hit.index;
        break;
      case longan::ui_click_type::play:
        if (hit.index >= 0 && hit.index < static_cast<int>(g_games.size()))
          g_pending_launch = g_games[hit.index].name;
        break;
      case longan::ui_click_type::select_setting:
        if (hit.index >= 0 && hit.index < g_set_count) {
          const bool same = (g_set_selected == hit.index);
          g_set_selected = hit.index;
          if ((g_set_rows[hit.index].is_bool || same) && edit_setting_row(1))
            apply_settings_rows();
          longan::ui_set_settings(g_set_rows, g_set_count, g_set_selected);
        }
        break;
      case longan::ui_click_type::none:
        break;
    }
  }

  void boot_all() {
    const std::string install = package_install_path();
    g_local_state =
        ws2u8(winrt::Windows::Storage::ApplicationData::Current()
                  .LocalFolder()
                  .Path());
    g_games_dir = g_local_state + "\\games";
    longan::log_open(install);
    char line[256];
    snprintf(line, sizeof(line), "tiramisu boot: pkg=%s state=%s",
             install.c_str(), g_local_state.c_str());
    longan::log_line(line);

    // ---- persisted settings (dashboard-editable; consumed by bridges) ----
    const int applied = longan::settings_load(g_local_state);
    snprintf(line, sizeof(line), "settings: %d keys applied from settings.ini",
             applied);
    longan::log_line(line);

    // System specs for overlay (read once at boot)
    {
      MEMORYSTATUSEX ms{};
      ms.dwLength = sizeof(ms);
      GlobalMemoryStatusEx(&ms);
      HKEY hkey;
      wchar_t cpuName[128] = L"Unknown CPU";
      if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                        L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0,
                        KEY_READ, &hkey) == ERROR_SUCCESS) {
        DWORD sz = sizeof(cpuName);
        RegQueryValueExW(hkey, L"ProcessorNameString", nullptr, nullptr,
                         (LPBYTE)cpuName, &sz);
        RegCloseKey(hkey);
      }
      wchar_t info[256];
      swprintf(info, 256, L"%s | %.0f GB RAM", cpuName,
               (double)ms.ullTotalPhys / (1024.0 * 1024.0 * 1024.0));
      longan::ui_set_sysinfo(info);
    }

    // ---- setup screen: steps update live as boot proceeds ----
    g_setup_active = true;
    auto setup_steps = [](std::initializer_list<std::wstring> done,
                          const wchar_t* current, bool finished) {
      std::vector<std::wstring> steps;
      for (const auto& d : done) steps.push_back(d);
      if (current) steps.push_back(current);
      longan::ui_set_setup(steps, (int)done.size(), finished);
    };
    setup_steps({L"Storage ready"}, L"Checking Direct3D 11 device…", false);

    // (the swapchain was created in SetWindow; report it as the D3D step)
    snprintf(line, sizeof(line), "setup: d3d11 ready (swapchain created in SetWindow)");
    longan::log_line(line);
    setup_steps({L"Storage ready", L"Direct3D 11 ready"}, L"Probing gamepads…",
                false);
    run_probes();
    setup_steps({L"Storage ready", L"Direct3D 11 ready", L"Gamepads probed"},
                L"Starting audio output…", false);
    longan::audio_init();
    longan::audio_status(line, sizeof(line));
    longan::log_line(line);
    setup_steps({L"Storage ready", L"Direct3D 11 ready", L"Gamepads probed",
                 std::wstring(L"Audio: ") + u82ws(line)},
                L"Starting the transfer server…", false);
    const std::string url = longan::webxfer_start(g_local_state, 1920);
    snprintf(line, sizeof(line), "transfer: %s (QR on TV)", url.c_str());
    longan::log_line(line);
    longan::ui_set_qr(url);
    const auto code = longan::webxfer_control_code();
    if (!url.empty() && !code.empty()) g_control_code_cached = code;
    refresh_control_hint();
    namespace wdi = winrt::Windows::Devices::Input;
    int keyboard = -1, mouse = -1;
    try { keyboard = wdi::KeyboardCapabilities().KeyboardPresent(); } catch (...) {}
    try { mouse = wdi::MouseCapabilities().MousePresent(); } catch (...) {}
    snprintf(line, sizeof(line), "input: Xbox device query keyboard=%d mouse=%d (-1 unavailable)",
             keyboard, mouse);
    longan::log_line(line);
    try {
      const auto pdevs = wdi::PointerDevice::GetPointerDevices();
      snprintf(line, sizeof(line), "input: PointerDevices count=%u",
               static_cast<unsigned>(pdevs.Size()));
      longan::log_line(line);
      for (const auto& pd : pdevs) {
        const auto rc = pd.ScreenRect();
        snprintf(line, sizeof(line), "input: PointerDevice type=%u integrated=%u rect=%.0fx%.0f",
                 static_cast<unsigned>(pd.PointerDeviceType()),
                 pd.IsIntegrated() ? 1u : 0u, rc.Width, rc.Height);
        longan::log_line(line);
      }
    } catch (...) {
      longan::log_line("input: PointerDevice::GetPointerDevices failed");
    }
    try {
      const auto rgc = winrt::Windows::Gaming::Input::RawGameController::RawGameControllers();
      snprintf(line, sizeof(line), "input: RawGameControllers count=%u",
               static_cast<unsigned>(rgc.Size()));
      longan::log_line(line);
    } catch (...) {
      longan::log_line("input: RawGameControllers failed");
    }
    try {
      const auto hids = winrt::Windows::Devices::Enumeration::DeviceInformation::FindAllAsync(
          L"System.Devices.InterfaceClassGuid:=\"{4D1E55B2-F16F-11CF-88CB-001111000030}\"").get();
      snprintf(line, sizeof(line), "input: WinRT HID interfaces count=%u",
               static_cast<unsigned>(hids.Size()));
      longan::log_line(line);
    } catch (...) {
      longan::log_line("input: WinRT HID enum unavailable");
    }
    const std::wstring url_step = url.empty() ? L"Transfer unavailable"
                                              : L"Transfer server ready";
    setup_steps({L"Storage ready", L"Direct3D 11 ready", L"Gamepads probed",
                 L"Audio ready", url_step},
                L"Scanning for games…", false);
    rescan_games();
    setup_steps({L"Storage ready", L"Direct3D 11 ready", L"Gamepads probed",
                 L"Audio ready", url_step, L"Game shelf scanned"},
                L"Running the built-in self-test…", false);

    // builtin self-test: proves the whole pipeline on hardware.
    // guest_hello imports tiramisu_host.dll!TiramisuHostLogU32 — the BUILTIN
    // provider must be registered or the selftest fails rc=IMPORT on console
    // (the offline tests register their own; the host app never did).
    static const trt_named_fn k_host_builtin_fns[] = {
        {"TiramisuHostLogU32", (void*)&host_builtin_log_u32},
        {"TiramisuHostQueryBuild", (void*)&host_builtin_query_build},
    };
    trt_module_register("tiramisu_host.dll", TRT_ROUTE_BUILTIN,
                        k_host_builtin_fns, 2);
    const auto gr = run_game("selftest", "guest_hello.exe", package_install_path());
    snprintf(line, sizeof(line), "selftest: rc=%s (%s)", trt_errstr(gr.map_rc),
             gr.detail);
    longan::log_line(line);
    set_guest_status(std::string("selftest ") + trt_errstr(gr.map_rc));
    setup_steps({L"Storage ready", L"Direct3D 11 ready", L"Gamepads probed",
                 L"Audio ready", url_step, L"Game shelf scanned",
                 L"Self-test: " + u82ws(trt_errstr(gr.map_rc))},
                nullptr, true);
    g_setup_done = true;  // A press on the finished screen opens the shelf
  }

  void enable_keyboard_interceptor() {
    // Xbox can reserve system keyboard processing even while this app has focus.
    // The foreground interceptor routes raw physical key events here instead of
    // CoreWindow.KeyDown/Up and lets the Win32 guest receive the same key stream.
    namespace wui = winrt::Windows::UI::Input;
    try {
      m_keyboard_interceptor = wui::KeyboardDeliveryInterceptor::GetForCurrentView();
      if (m_keyboard_interceptor) {
        m_keyboard_interceptor.IsInterceptionEnabledWhenInForeground(true);
        m_ikd = m_keyboard_interceptor.KeyDown(
            [](wui::KeyboardDeliveryInterceptor const&, KeyEventArgs const& a) {
          const auto status = a.KeyStatus();
          u16 scan = static_cast<u16>(status.ScanCode);
          bool ext = status.IsExtendedKey;
          const u16 raw_vk = static_cast<u16>(a.VirtualKey());
          if (!scan) {
            const u32 mv = trt_MapVirtualKeyW(raw_vk, 4 /*MAPVK_VK_TO_VSC_EX*/);
            if (mv) {
              scan = static_cast<u16>(mv & 0xFFu);
              if ((mv & 0xFF00u) == 0xE000u) ext = true;
            }
          }
          const u16 vk = disambiguate_modifier_vk(raw_vk, scan, ext);
          g_interceptor_kbd_active.store(true);
          g_native_input.notify_interceptor_key(vk);
          g_native_input.notify_interceptor_key(raw_vk);
          if (handle_host_cursor_key(vk, scan, ext, true)) {
            a.Handled(true);
            return;
          }
          trt_input_key_event2(vk, true, scan, ext ? 1 : 0, status.WasKeyDown ? 1 : 0);
          const bool shortcut = trt_key_down(0x11) || trt_key_down(0x12);
          if (!shortcut) {
            const u32 ch = trt_vk_to_char_ex(vk, nullptr);
            if (ch == '\b' || ch == '\t' || ch == '\r' || ch >= 32)
              trt_input_char_event(ch);
          }
          a.Handled(true);
          static bool logged = false;
          if (!logged) {
            logged = true;
            longan::log_line("input: foreground keyboard event received");
          }
        });
        m_iku = m_keyboard_interceptor.KeyUp(
            [](wui::KeyboardDeliveryInterceptor const&, KeyEventArgs const& a) {
          const auto status = a.KeyStatus();
          u16 scan = static_cast<u16>(status.ScanCode);
          bool ext = status.IsExtendedKey;
          const u16 raw_vk = static_cast<u16>(a.VirtualKey());
          if (!scan) {
            const u32 mv = trt_MapVirtualKeyW(raw_vk, 4 /*MAPVK_VK_TO_VSC_EX*/);
            if (mv) {
              scan = static_cast<u16>(mv & 0xFFu);
              if ((mv & 0xFF00u) == 0xE000u) ext = true;
            }
          }
          const u16 vk = disambiguate_modifier_vk(raw_vk, scan, ext);
          g_interceptor_kbd_active.store(true);
          g_native_input.notify_interceptor_key(vk);
          g_native_input.notify_interceptor_key(raw_vk);
          if (handle_host_cursor_key(vk, scan, ext, false)) {
            a.Handled(true);
            return;
          }
          trt_input_key_event2(vk, false, scan, ext ? 1 : 0, 0);
          a.Handled(true);
        });
        longan::log_line(m_keyboard_interceptor.IsInterceptionEnabledWhenInForeground()
                             ? "input: foreground keyboard interception enabled"
                             : "input: foreground keyboard interception unavailable");
      } else {
        longan::log_line("input: foreground keyboard interceptor unavailable");
      }
    } catch (...) {
      longan::log_line("input: foreground keyboard interception denied");
    }
  }

  void setup_mouse_device() {
    namespace wdi = winrt::Windows::Devices::Input;
    try {
      m_mouse_device = wdi::MouseDevice::GetForCurrentView();
      if (m_mouse_device) {
        m_mm = m_mouse_device.MouseMoved(
            [](wdi::MouseDevice const&, wdi::MouseEventArgs const& a) {
          if (g_native_input.mouse_active()) return;
          if (!g_uwp_raw_mouse.exchange(true))
            longan::log_line("input: physical relative mouse motion received");
          const auto d = a.MouseDelta();
          trt_input_mouse_delta_raw(static_cast<i32>(d.X), static_cast<i32>(d.Y));
        });
        m_mouse_device_available = true;
        longan::log_line("input: MouseDevice relative listener registered");
      } else {
        m_mouse_device_available = false;
        longan::log_line("input: MouseDevice unavailable; CoreWindow pointer listener active");
      }
    } catch (...) {
      m_mouse_device_available = false;
      longan::log_line("input: MouseDevice registration failed; CoreWindow pointer listener active");
    }
  }

  void Run() {
    g_ui_tid = GetCurrentThreadId();
    auto active_window = CoreWindow::GetForCurrentThread();
    active_window.Activate();
    if (!g_booted) {
      g_booted = true;
      boot_all();
      g_native_input.set_host_key_handler(&handle_host_cursor_key);
      g_native_input.open();
      longan::set_pointer_cursor_mode(false);
    }
    setup_mouse_device();
    longan::log_line(active_window.IsInputEnabled()
                         ? "input: CoreWindow initially enabled"
                         : "input: CoreWindow initially disabled");
    longan::log_line(m_mouse_device_available
                         ? "input: MouseDevice relative listener registered"
                         : "input: MouseDevice unavailable; CoreWindow pointer listener active");
    enable_keyboard_interceptor();

    int mouse_vx = 960, mouse_vy = 540;
    while (!g_shutdown.load()) {
      trt_input_frame_begin();
      CoreWindow::GetForCurrentThread()
          .Dispatcher()
          .ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
      if (g_input_focused && g_native_input.poll()) launcher_click();
      apply_web_controls();

      // ---- controller & keyboard launcher navigation ----
      XINPUT_STATE xs{};
      bool pad_ok = false;
      poll_host_gamepads(&xs, &pad_ok);
      {
        static bool d_up = false, d_dn = false, d_lt = false, d_rt = false,
                    a_btn = false, y_btn = false, rs_btn = false, vkbd_btn = false;
        static bool lb_btn = false, rb_btn = false;
        static bool mm_was_active = false, mm_was_rt = false, mm_was_lt = false,
                    mm_was_rb = false, mm_was_lb = false, mm_was_ls = false,
                    mm_was_a = false, mm_was_b = false, mm_was_x = false,
                    mm_was_y = false, mm_was_dl = false, mm_was_dr = false,
                    mm_was_w = false, mm_was_s = false, mm_was_ak = false,
                    mm_was_d = false, guest_was_running = false;
        static int mm_wheel_tick = 0, vm_hold_ticks = 0;
        const u16 b = pad_ok ? xs.Gamepad.wButtons : 0;
        // RB/LB cycle dashboard screens (shelf -> settings -> log) on the
        // launcher; keyboard: Tab / Shift+Tab.
        const bool want_rb = (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) ||
                             (launcher_key(0x09 /*VK_TAB*/) &&
                              !trt_key_down(0x10 /*VK_SHIFT*/));
        const bool want_lb = (b & XINPUT_GAMEPAD_LEFT_SHOULDER) ||
                             (trt_key_down(0x10 /*VK_SHIFT*/) &&
                              launcher_key(0x09 /*VK_TAB*/));
        if (!g_guest_running.load() && g_setup_active == false) {
          if (want_rb && !rb_btn) {
            rb_btn = true;
            g_screen = (longan::ui_screen)(((int)g_screen + 1) % longan::UI_TAB_COUNT);
            longan::ui_set_screen(g_screen);
            if (g_screen == longan::UI_SETTINGS) {
              sync_settings_rows();
              longan::ui_set_settings(g_set_rows, g_set_count, g_set_selected);
            }
          } else if (!want_rb) rb_btn = false;
          if (want_lb && !lb_btn) {
            lb_btn = true;
            g_screen = (longan::ui_screen)(((int)g_screen + longan::UI_TAB_COUNT - 1) %
                                          longan::UI_TAB_COUNT);
            longan::ui_set_screen(g_screen);
            if (g_screen == longan::UI_SETTINGS) {
              sync_settings_rows();
              longan::ui_set_settings(g_set_rows, g_set_count, g_set_selected);
            }
          } else if (!want_lb) lb_btn = false;
        }
        static bool km_was_lmb = false, km_was_rmb = false,
                    km_was_mmb = false, alt_f4_was = false;
        static int km_wheel_tick = 0;
        static ULONGLONG km_last_ms = 0;
        static float km_rem_x = 0.0f, km_rem_y = 0.0f;
        const ULONGLONG km_now_ms = GetTickCount64();
        const float km_dt = km_last_ms
            ? std::min(0.05f, static_cast<float>(km_now_ms - km_last_ms) / 1000.0f)
            : (1.0f / 60.0f);
        km_last_ms = km_now_ms;
        if (g_f8_pressed.exchange(false)) {
          g_kbd_mouse_mode = !g_kbd_mouse_mode;
          for (auto& k : g_km_keys) k.store(false);
          km_rem_x = km_rem_y = 0.0f;
          longan::set_pointer_cursor_mode(g_kbd_mouse_mode);
          refresh_control_hint();
          longan::log_line(g_kbd_mouse_mode ? "input: keyboard-mouse cursor mode ON (F8)"
                                            : "input: keyboard-mouse cursor mode OFF (F8)");
        }
        {
          const bool shift_held = trt_key_down(0x10 /*VK_SHIFT*/) ||
                                  trt_key_down(0xA0) || trt_key_down(0xA1);
          const bool ctrl_held = trt_key_down(0xA2 /*VK_LCONTROL*/) ||
                                 (!g_kbd_mouse_mode && trt_key_down(0x11));
          const float speed = shift_held ? 1100.0f : (ctrl_held ? 180.0f : 560.0f);
          float sx = 0.0f, sy = 0.0f;
          if (km_key_held(0x64 /*VK_NUMPAD4*/) || km_key_held(0x67 /*VK_NUMPAD7*/) ||
              km_key_held(0x61 /*VK_NUMPAD1*/) ||
              (g_kbd_mouse_mode && km_key_held(0x25 /*VK_LEFT*/)))
            sx -= 1.0f;
          if (km_key_held(0x66 /*VK_NUMPAD6*/) || km_key_held(0x69 /*VK_NUMPAD9*/) ||
              km_key_held(0x63 /*VK_NUMPAD3*/) ||
              (g_kbd_mouse_mode && km_key_held(0x27 /*VK_RIGHT*/)))
            sx += 1.0f;
          if (km_key_held(0x68 /*VK_NUMPAD8*/) || km_key_held(0x67 /*VK_NUMPAD7*/) ||
              km_key_held(0x69 /*VK_NUMPAD9*/) ||
              (g_kbd_mouse_mode && km_key_held(0x26 /*VK_UP*/)))
            sy -= 1.0f;
          if (km_key_held(0x62 /*VK_NUMPAD2*/) || km_key_held(0x61 /*VK_NUMPAD1*/) ||
              km_key_held(0x63 /*VK_NUMPAD3*/) ||
              (g_kbd_mouse_mode && km_key_held(0x28 /*VK_DOWN*/)))
            sy += 1.0f;
          if (sx != 0.0f || sy != 0.0f) {
            const float diag = (sx != 0.0f && sy != 0.0f) ? 0.7071f : 1.0f;
            km_rem_x += sx * diag * speed * km_dt;
            km_rem_y += sy * diag * speed * km_dt;
            const int dx = static_cast<int>(km_rem_x);
            const int dy = static_cast<int>(km_rem_y);
            km_rem_x -= static_cast<float>(dx);
            km_rem_y -= static_cast<float>(dy);
            if (dx || dy) trt_input_mouse_delta_raw(dx, dy);
          } else {
            km_rem_x = km_rem_y = 0.0f;
          }

          const bool lmb = km_key_held(0x60 /*VK_NUMPAD0*/) ||
                           (g_kbd_mouse_mode &&
                            (km_key_held(0x0D /*VK_RETURN*/) || km_key_held(0x20 /*VK_SPACE*/)));
          const bool rmb = km_key_held(0x6E /*VK_DECIMAL*/) ||
                           (g_kbd_mouse_mode &&
                            (km_key_held(0xA3 /*VK_RCONTROL*/) || km_key_held(0x5D /*VK_APPS*/)));
          const bool mmb = km_key_held(0x65 /*VK_NUMPAD5*/);
          if (lmb != km_was_lmb) {
            trt_input_mouse_button(0, lmb);
            if (!lmb && !g_guest_running.load()) launcher_click();
            km_was_lmb = lmb;
          }
          if (rmb != km_was_rmb) {
            trt_input_mouse_button(1, rmb);
            km_was_rmb = rmb;
          }
          if (mmb != km_was_mmb) {
            trt_input_mouse_button(2, mmb);
            km_was_mmb = mmb;
          }
          const bool wheel_up = km_key_held(0x6B /*VK_ADD*/) ||
                                (g_kbd_mouse_mode && km_key_held(0x21 /*VK_PRIOR*/));
          const bool wheel_dn = km_key_held(0x6D /*VK_SUBTRACT*/) ||
                                (g_kbd_mouse_mode && km_key_held(0x22 /*VK_NEXT*/));
          if (wheel_up) {
            if (km_wheel_tick <= 0) { trt_input_wheel(120); km_wheel_tick = 10; }
            else --km_wheel_tick;
          } else if (wheel_dn) {
            if (km_wheel_tick <= 0) { trt_input_wheel(-120); km_wheel_tick = 10; }
            else --km_wheel_tick;
          } else {
            km_wheel_tick = 0;
          }
        }
        const bool alt_f4 = g_guest_running.load() &&
                            trt_key_down(0x12 /*VK_MENU*/) &&
                            trt_key_down(0x73 /*VK_F4*/);
        if (alt_f4 && !alt_f4_was) {
          alt_f4_was = true;
          request_guest_stop();
        } else if (!alt_f4) {
          alt_f4_was = false;
        }

        const bool km_lock = g_kbd_mouse_mode;
        const bool want_up = (b & XINPUT_GAMEPAD_DPAD_UP) ||
                             (!km_lock && (launcher_key(0x26 /*VK_UP*/) || launcher_key('W')));
        const bool want_dn = (b & XINPUT_GAMEPAD_DPAD_DOWN) ||
                             (!km_lock && (launcher_key(0x28 /*VK_DOWN*/) || launcher_key('S')));
        const bool want_lt = (b & XINPUT_GAMEPAD_DPAD_LEFT) ||
                             (!km_lock && launcher_key(0x25 /*VK_LEFT*/));
        const bool want_rt = (b & XINPUT_GAMEPAD_DPAD_RIGHT) ||
                             (!km_lock && launcher_key(0x27 /*VK_RIGHT*/));
        const bool want_go = (b & XINPUT_GAMEPAD_A) ||
                             (!km_lock && (launcher_key(0x0D /*VK_RETURN*/) ||
                                           launcher_key(0x20 /*VK_SPACE*/)));
        const bool want_st = (b & XINPUT_GAMEPAD_Y) || (!km_lock && launcher_key('Y'));
        const bool pad_vm = (b & XINPUT_GAMEPAD_BACK) && (b & XINPUT_GAMEPAD_START);
        if (g_guest_running.load()) {
          if (longan::webxfer_poll_stop()) request_guest_stop();
          if (longan::trt_settings_quick_menu_enabled() && pad_vm) {
            // RetroArch-style: Back+Start toggles the quick menu
            static bool qm_was = false;
            if (!qm_was) {
              qm_was = true;
              g_quick_menu_open = !g_quick_menu_open;
              longan::ui_set_quick_menu(g_quick_menu_open);
            }
          } else {
            // hold Back+Start for 45 ticks to force-stop (escape hatch)
            if (pad_vm) {
              if (++vm_hold_ticks == 45) request_guest_stop();
            } else {
              vm_hold_ticks = 0;
            }
          }
          if (!pad_vm) {
            static bool qm_reset = true;
            qm_reset = true;
          }
        } else {
          vm_hold_ticks = 0;
          longan::webxfer_poll_stop();
          if (guest_was_running) {
            trt_input_release_all();
            trt_input_reset_cursor_mode(false);
            g_mouse_mode = false;
            longan::webxfer_set_running_game("");
          }
        }
        guest_was_running = g_guest_running.load();
        const bool want_mm = (b & XINPUT_GAMEPAD_RIGHT_THUMB) ||
                             (!g_guest_running.load() && pad_vm) ||
                             trt_key_down(0x70 /*VK_F1*/);
        const bool want_vkbd = (b & XINPUT_GAMEPAD_BACK) && (b & XINPUT_GAMEPAD_X);
        if (want_vkbd && !vkbd_btn) {
          vkbd_btn = true;
          try {
            winrt::Windows::UI::ViewManagement::InputPane::GetForCurrentView().TryShow();
          } catch (...) {}
        } else if (!want_vkbd) {
          vkbd_btn = false;
        }
        if (!g_guest_running.load() && g_screen == longan::UI_SHELF) {
          if (want_up && !d_up) {
            d_up = true;
            if (g_selected > 0) --g_selected;
          } else if (!want_up) d_up = false;
          if (want_dn && !d_dn) {
            d_dn = true;
            if (g_selected + 1 < (int)g_games.size()) ++g_selected;
          } else if (!want_dn) d_dn = false;
          if (!g_setup_active && !g_games.empty() && g_selected < (int)g_games.size()) {
            auto& ge = g_games[g_selected];
            const int n = (int)ge.exes.size();
            if (want_lt && !d_lt) {
              d_lt = true;
              if (n > 1) {
                ge.exe_idx = (ge.exe_idx + n - 1) % n;
                ge.exe = ge.exes[ge.exe_idx];
                char compat[280];
                scan_compat(ge.dir + "\\" + ge.exe, compat,
                            (unsigned)sizeof(compat));
                ge.compat = compat;
              }
            } else if (!want_lt) d_lt = false;
            if (want_rt && !d_rt) {
              d_rt = true;
              if (n > 1) {
                ge.exe_idx = (ge.exe_idx + 1) % n;
                ge.exe = ge.exes[ge.exe_idx];
                char compat[280];
                scan_compat(ge.dir + "\\" + ge.exe, compat,
                            (unsigned)sizeof(compat));
                ge.compat = compat;
              }
            } else if (!want_rt) d_rt = false;
          } else {
            d_lt = d_rt = false;
          }
          if (want_go && !a_btn) {
            a_btn = true;
            if (g_setup_active && g_setup_done) {
              g_setup_active = false;  // leave setup, open the shelf
              longan::ui_dismiss_first_run();
            } else if (!g_setup_active && !g_games.empty()) {
              g_pending_launch = g_games[g_selected].name;
            }
          } else if (!want_go) a_btn = false;
          if (want_st && !y_btn) {
            y_btn = true;
            steam_begin();
          } else if (!want_st) y_btn = false;
        } else if (!g_guest_running.load() && g_screen == longan::UI_SETTINGS) {
          // settings screen nav: up/down select, LR edit + persist, A toggle
          if (want_up && !d_up) {
            d_up = true;
            if (g_set_selected > 0) {
              --g_set_selected;
              longan::ui_set_settings(g_set_rows, g_set_count, g_set_selected);
            }
          } else if (!want_up) d_up = false;
          if (want_dn && !d_dn) {
            d_dn = true;
            if (g_set_selected + 1 < g_set_count) {
              ++g_set_selected;
              longan::ui_set_settings(g_set_rows, g_set_count, g_set_selected);
            }
          } else if (!want_dn) d_dn = false;
          if (want_lt && !d_lt) {
            d_lt = true;
            if (edit_setting_row(-1)) apply_settings_rows();
          } else if (!want_lt) d_lt = false;
          if (want_rt && !d_rt) {
            d_rt = true;
            if (edit_setting_row(1)) apply_settings_rows();
          } else if (!want_rt) d_rt = false;
          if (want_go && !a_btn) {
            a_btn = true;
            if (edit_setting_row(1)) apply_settings_rows();
          } else if (!want_go) a_btn = false;
        } else {
          d_up = d_dn = d_lt = d_rt = a_btn = y_btn = false;
        }
        if (want_mm && !rs_btn) {
          rs_btn = true;
          g_mouse_mode = !g_mouse_mode;
        } else if (!want_mm) rs_btn = false;

        // gamepad-as-mouse + virtual KBM (KBM games without a real mouse/KB)
        if (pad_ok && g_mouse_mode) {
          mm_was_active = true;
          const int dz = 8000;
          int sx = xs.Gamepad.sThumbRX;
          int sy = -xs.Gamepad.sThumbRY;
          sx = sx > dz ? sx - dz : sx < -dz ? sx + dz : 0;
          sy = sy > dz ? sy - dz : sy < -dz ? sy + dz : 0;
          const int ddx = (int)(sx / 32767.0f * 1400.0f / 60.0f);
          const int ddy = (int)(sy / 32767.0f * 1400.0f / 60.0f);
          int dw = 1920, dh = 1080;
          trt_display_size(&dw, &dh);
          if (dw <= 0) dw = 1920;
          if (dh <= 0) dh = 1080;
          mouse_vx += ddx;
          mouse_vy += ddy;
          if (mouse_vx < 0) mouse_vx = 0;
          if (mouse_vx >= dw) mouse_vx = dw - 1;
          if (mouse_vy < 0) mouse_vy = 0;
          if (mouse_vy >= dh) mouse_vy = dh - 1;
          trt_input_pointer_move(mouse_vx, mouse_vy, true);
          if (ddx != 0 || ddy != 0) trt_input_mouse_delta_raw(ddx, ddy);

          const bool rt = xs.Gamepad.bRightTrigger >
                          XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
          const bool lt = xs.Gamepad.bLeftTrigger >
                          XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
          const bool rb = (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
          if (rt != mm_was_rt) { trt_input_mouse_button(0, rt); mm_was_rt = rt; }
          if (lt != mm_was_lt) { trt_input_mouse_button(1, lt); mm_was_lt = lt; }
          if (rb != mm_was_rb) { trt_input_mouse_button(2, rb); mm_was_rb = rb; }

          if (g_guest_running.load()) {
            const bool k_w = xs.Gamepad.sThumbLY > 14000;
            const bool k_s = xs.Gamepad.sThumbLY < -14000;
            const bool k_d = xs.Gamepad.sThumbLX > 14000;
            const bool k_a = xs.Gamepad.sThumbLX < -14000;
            const bool k_ent = (b & XINPUT_GAMEPAD_A) != 0;
            const bool k_esc = (b & XINPUT_GAMEPAD_B) != 0;
            const bool k_spc = (b & XINPUT_GAMEPAD_X) != 0;
            const bool k_e = (b & XINPUT_GAMEPAD_Y) != 0;
            const bool k_tab = (b & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
            const bool k_shf = (b & XINPUT_GAMEPAD_LEFT_THUMB) != 0;
            const bool k_r = (b & XINPUT_GAMEPAD_DPAD_LEFT) != 0;
            const bool k_f = (b & XINPUT_GAMEPAD_DPAD_RIGHT) != 0;
            if (k_w != mm_was_w) { trt_input_key_event('W', k_w); mm_was_w = k_w; }
            if (k_s != mm_was_s) { trt_input_key_event('S', k_s); mm_was_s = k_s; }
            if (k_a != mm_was_ak) { trt_input_key_event('A', k_a); mm_was_ak = k_a; }
            if (k_d != mm_was_d) { trt_input_key_event('D', k_d); mm_was_d = k_d; }
            if (k_ent != mm_was_a) { trt_input_key_event(0x0D, k_ent); mm_was_a = k_ent; }
            if (k_esc != mm_was_b) { trt_input_key_event(0x1B, k_esc); mm_was_b = k_esc; }
            if (k_spc != mm_was_x) { trt_input_key_event(0x20, k_spc); mm_was_x = k_spc; }
            if (k_e != mm_was_y) { trt_input_key_event('E', k_e); mm_was_y = k_e; }
            if (k_tab != mm_was_lb) { trt_input_key_event(0x09, k_tab); mm_was_lb = k_tab; }
            if (k_shf != mm_was_ls) { trt_input_key_event(0xA0, k_shf); mm_was_ls = k_shf; }
            if (k_r != mm_was_dl) { trt_input_key_event('R', k_r); mm_was_dl = k_r; }
            if (k_f != mm_was_dr) { trt_input_key_event('F', k_f); mm_was_dr = k_f; }
            if (b & XINPUT_GAMEPAD_DPAD_UP) {
              if (mm_wheel_tick <= 0) { trt_input_wheel(120); mm_wheel_tick = 8; }
              else --mm_wheel_tick;
            } else if (b & XINPUT_GAMEPAD_DPAD_DOWN) {
              if (mm_wheel_tick <= 0) { trt_input_wheel(-120); mm_wheel_tick = 8; }
              else --mm_wheel_tick;
            } else {
              mm_wheel_tick = 0;
            }
          }
        } else if (mm_was_active) {
          mm_was_active = false;
          if (mm_was_rt) { trt_input_mouse_button(0, false); mm_was_rt = false; }
          if (mm_was_lt) { trt_input_mouse_button(1, false); mm_was_lt = false; }
          if (mm_was_rb) { trt_input_mouse_button(2, false); mm_was_rb = false; }
          if (mm_was_w) { trt_input_key_event('W', false); mm_was_w = false; }
          if (mm_was_s) { trt_input_key_event('S', false); mm_was_s = false; }
          if (mm_was_ak) { trt_input_key_event('A', false); mm_was_ak = false; }
          if (mm_was_d) { trt_input_key_event('D', false); mm_was_d = false; }
          if (mm_was_a) { trt_input_key_event(0x0D, false); mm_was_a = false; }
          if (mm_was_b) { trt_input_key_event(0x1B, false); mm_was_b = false; }
          if (mm_was_x) { trt_input_key_event(0x20, false); mm_was_x = false; }
          if (mm_was_y) { trt_input_key_event('E', false); mm_was_y = false; }
          if (mm_was_lb) { trt_input_key_event(0x09, false); mm_was_lb = false; }
          if (mm_was_ls) { trt_input_key_event(0xA0, false); mm_was_ls = false; }
          if (mm_was_dl) { trt_input_key_event('R', false); mm_was_dl = false; }
          if (mm_was_dr) { trt_input_key_event('F', false); mm_was_dr = false; }
        }
      }

      // ---- launch handoff (web + pad) ----
      if (!g_guest_running.load()) {
        if (longan::webxfer_poll_rescan() || m_frames - m_rescan_at >= 120) {
          m_rescan_at = m_frames;
          rescan_games();
        }
        std::string webname;
        if (longan::webxfer_poll_launch(&webname)) {
          g_pending_launch = webname;
          if (g_setup_active && g_setup_done) {
            g_setup_active = false;
            longan::ui_dismiss_first_run();
          }
          bool found = false;
          for (const auto& ge : g_games)
            if (ge.name == webname) { found = true; break; }
          if (!found) rescan_games();
        }
        if (!g_pending_launch.empty()) {
          const std::string want = g_pending_launch;
          g_pending_launch.clear();
          bool shelf_hit = false;
          for (const auto& ge : g_games)
            if (ge.name == want) { shelf_hit = true; break; }
          if (!shelf_hit) {
            // console reality: the shelf's FindFirstFileA scan can come up
            // empty while the web dashboard's enumeration lists the game —
            // fall back to the webxfer's own list so a web launch NEVER
            // drops silently
            char wexe[1024] = {};
            if (longan::webxfer_find_game(want.c_str(), wexe, sizeof(wexe))) {
              std::string dir_rel = want;
              for (char& c : dir_rel) if (c == '/') c = '\\';
              game_entry fge;
              fge.name = want;
              fge.exe = wexe;
              fge.exes.push_back(wexe);
              fge.dir = g_games_dir + std::string(1, '\\') + dir_rel;
              fge.input_mode = "auto";
              char found_line[192];
              snprintf(found_line, sizeof(found_line),
                       "shelf: %s -> %s (web list fallback)", want.c_str(), wexe);
              longan::log_line(found_line);
              g_games.push_back(std::move(fge));
            } else {
              char drop[160];
              snprintf(drop, sizeof(drop),
                       "launch %s: game not found on shelf or web list", want.c_str());
              longan::log_line(drop);
            }
          }
          for (const auto& ge : g_games) {
            if (ge.name == want) {
              if (g_guest_thread.joinable()) g_guest_thread.join();
              set_guest_status("running " + ge.name + "…");
              if (ge.input_mode == "pad2kbm" || ge.input_mode == "kbm") {
                g_mouse_mode = true;
              } else if (ge.input_mode == "controller") {
                g_mouse_mode = false;
              }
              trt_input_release_all();
              // Keep the visible console cursor active for all games unless
              // the guest explicitly pins it to a 1x1 mouselook trap.
              trt_input_reset_cursor_mode(true);
              longan::webxfer_set_running_game(want);
              g_guest_running.store(true);
              const std::string exe = ge.exe;
              const std::string gdir = ge.dir;
              const std::string img = ge.image;
              g_guest_thread = std::thread([want, exe, gdir, img]() {
                const auto gr = run_game(want, exe, gdir, img);
                char line[192];
                snprintf(line, sizeof(line), "launch %s: rc=%s (%s)",
                         want.c_str(), trt_errstr(gr.map_rc), gr.detail);
                longan::log_line(line);
                set_guest_status(want + ": " + (gr.map_rc == TRT_OK ? gr.detail
                                                                   : trt_errstr(gr.map_rc)));
                longan::webxfer_set_running_game("");
                g_guest_running.store(false);
              });
              break;
            }
          }
        }
      }

      trt_openal_pump(8);

      // While a guest owns the host swapchain, do not touch D3D11/DXGI from the UI
      // loop; keep pumping CoreWindow events, XInput, virtual mouse, and OpenAL.
      // EXCEPTION: if the quick menu is open, render it on top of the game.
      if (g_guest_running.load() && !g_quick_menu_open) {
        Sleep(4);
        continue;
      }
      if (g_guest_running.load() && g_quick_menu_open) {
        // quick menu navigation — this branch runs on the UI loop, NOT in the
        // pad-poll block above (whose locals are out of scope here), so poll
        // the pad fresh.
        static bool qm_up = false, qm_dn = false, qm_go = false, qm_back = false;
        XINPUT_STATE qm_xs{};
        ZeroMemory(&qm_xs, sizeof(qm_xs));
        const u16 qm_b = (XInputGetState(0, &qm_xs) == ERROR_SUCCESS)
                             ? qm_xs.Gamepad.wButtons
                             : 0;
        const bool want_up = (qm_b & XINPUT_GAMEPAD_DPAD_UP) != 0;
        const bool want_dn = (qm_b & XINPUT_GAMEPAD_DPAD_DOWN) != 0;
        const bool want_go = (qm_b & XINPUT_GAMEPAD_A) != 0;
        const bool pad_vm =
            (qm_b & XINPUT_GAMEPAD_BACK) && (qm_b & XINPUT_GAMEPAD_START);
        if (want_up && !qm_up) { qm_up = true; /* navigate up */ }
        else if (!want_up) qm_up = false;
        if (want_dn && !qm_dn) { qm_dn = true; /* navigate down */ }
        else if (!want_dn) qm_dn = false;
        if (want_go && !qm_go) {
          qm_go = true;
          // A: activate selected item (placeholder)
        } else if (!want_go) qm_go = false;
        // B or Back+Start again: close menu
        if ((qm_b & XINPUT_GAMEPAD_B) && !qm_back) {
          qm_back = true;
          g_quick_menu_open = false;
          longan::ui_set_quick_menu(false);
        } else if (!(qm_b & XINPUT_GAMEPAD_B)) qm_back = false;
        static bool qm_toggle = false;
        if (pad_vm) {
          if (!qm_toggle) {
            qm_toggle = true;
            g_quick_menu_open = false;
            longan::ui_set_quick_menu(false);
          }
        } else {
          qm_toggle = false;
        }
        // render quick menu overlay on top of the frozen game frame
        longan::swapchain_restore_d3d11();
        longan::ui_set_screen(longan::UI_QUICKMENU);
        longan::ui_render();
        longan::swapchain_present();
        Sleep(16);  // ~60fps for menu
        continue;
      }
      longan::swapchain_restore_d3d11();
      longan::swapchain_restore_launcher_format();

      // ---- UI ----
      // FPS tracking for overlay
      static auto fps_last = std::chrono::steady_clock::now();
      static int fps_frames = 0;
      fps_frames++;
      auto now = std::chrono::steady_clock::now();
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - fps_last).count();
      if (elapsed >= 1000) {
        longan::ui_set_fps(fps_frames * 1000.0f / elapsed);
        fps_frames = 0;
        fps_last = now;
      }
      const std::string transfer_url = longan::webxfer_url();
      longan::ui_set_transfer(u82ws(transfer_url),
                               u82ws(longan::webxfer_status_line()));
      const bool showing_setup = g_setup_active || g_screen == longan::UI_SETUP;
      longan::ui_set_qr(showing_setup ? transfer_url
                        : g_steam_busy ? g_steam_qr
                        : longan::settings_get().show_qr ? transfer_url
                        : std::string());
      if (!g_setup_active) {
        std::vector<longan::ui_game> uig;
        for (const auto& ge : g_games) {
          std::wstring disp = u82ws(ge.exe);
          if (!ge.image.empty()) disp = L"[disc] " + disp;
          if (ge.exes.size() > 1) {
            disp = L"[" + std::to_wstring(ge.exe_idx + 1) + L"/" +
                   std::to_wstring(ge.exes.size()) + L"] " + disp;
          }
          uig.push_back({u82ws(ge.name), disp, u82ws(ge.input_mode)});
        }
        longan::ui_set_games(uig, g_selected);
        std::vector<std::wstring> lines;
        lines.push_back(u82ws(longan::webxfer_status_line()));
        lines.push_back(u82ws(get_guest_status()));
        lines.push_back(u82ws(g_steam_status +
                              (g_mouse_mode ? "  |  PAD MOUSE (F1)" : "") +
                              (g_kbd_mouse_mode ? "  |  KBM CURSOR (F8)" : "")));
        char audio_line[128];
        longan::audio_status(audio_line, sizeof(audio_line));
        lines.push_back(u82ws(audio_line));
        // the selected game's static compat scan: what will happen on launch
        if (!g_games.empty() && g_selected < (int)g_games.size()) {
          lines.push_back(u82ws(g_games[g_selected].compat));
        }
        lines.push_back(
            L"RB/LB or Tab: screens  U/D: game  L/R: exe  A/Enter: launch  Y: steam");
        lines.push_back(
            L"F8: KBM cursor (Arrows+Space)  R3/F1: pad mouse  Hold View+Menu: stop");
        longan::ui_set_lines(lines);
      }
      if (g_screen == longan::UI_LOG && !g_setup_active) {
        // live log tail on the LOG screen: pull the in-memory ring
        static char logbuf[16384];
        logbuf[0] = 0;
        longan::log_get_recent(logbuf, sizeof(logbuf));
        std::vector<std::wstring> tail;
        const char* p = logbuf;
        while (*p && tail.size() < 60) {
          const char* nl = strchr(p, '\n');
          std::string one = nl ? std::string(p, nl - p) : std::string(p);
          if (!one.empty()) tail.push_back(u82ws(one));
          if (!nl) break;
          p = nl + 1;
        }
        longan::ui_set_log_tail(tail);
      }
      longan::swapchain_render_frame((m_frames % 180) / 180.0f);
      longan::ui_render();
      longan::swapchain_present();
      ++m_frames;
    }
  }

  bool m_web_keys[256]{};
  bool m_web_buttons[5]{};
  bool m_mouse_device_available = false;
  winrt::Windows::Devices::Input::MouseDevice m_mouse_device{nullptr};
  winrt::Windows::UI::Input::KeyboardDeliveryInterceptor m_keyboard_interceptor{nullptr};
  winrt::event_token m_kd{}, m_ku{}, m_cr{}, m_pp{}, m_pr{}, m_pm{}, m_pw{},
      m_pe{}, m_mm{}, m_focus{}, m_input_enabled{}, m_ikd{}, m_iku{}, m_aka{};
};

} // namespace

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  init_apartment(apartment_type::multi_threaded);
  CoreApplication::Run(make<tiramisu_host>());
  return 0;
}
