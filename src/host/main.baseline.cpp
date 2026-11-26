// tiramisu/src/host/main.cpp
// Tiramisu host — the UWP "Game" app for Xbox Series Dev Mode that:
//   * boots the trt runtime and executes Windows PE64s in-process
//   * serves the phone transfer page (QR on TV -> upload game zips -> launch)
//   * feeds guests keyboard/mouse (CoreWindow events) + controller (XInput),
//     including gamepad-as-mouse for KBM-only games
//   * optional Steam QR login (own account; depot pipeline is M6)
#include "host_log.h"
#include "audio.h"
#include "boot_anim.h"
#include "swapchain.h"
#include "ui.h"
#include "ui_sfx.h"
#include "cover_art.h"
#include "webxfer.h"
#include "host_autopilot.h"
#include "settings.h"
#include "crash_report.h"
#include "../trt/trt.h"
#include "../trt/x86_cpu.h"
#include "../trt/mono_host.h"
#include "../trt/fna_platform.h"
#include "../trt/fna3d_host.h"
#include "../trt/trt_trace.h"
#include "FNA3D.h"
#include "../trt/bridge_res.h"
#include "../trt/bridge_nvapi.h"
#include "../trt/iso.h"
#include "../trt/input.h"
#include "../trt/images.h"
#include "../trt/bridge_mem.h"
#include "../trt/bridge_d3d.h"
#include "../trt/bridge_openal.h"
#include "../trt/bridge_reg.h"
#include "../trt/engine_synthesizer.h"
#include "../trt/bridge_opengl32.h"
#include "../trt/steam_library.h"
#include "../trt/pc2xb_translator.h"
#include "../trt/dynamic_api.h"
#include "../trt/bridge_fmod_studio.h"
#include "../trt/bridge_steam.h"
#include "steam_client.h"
#include "../trt/spirv_hlsl.h"
#include "console_detect.h"
#include "../trt/bridge_esram.h"

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

// timeBeginPeriod is desktop-partition in the SDK headers (not declared
// under WINAPI_FAMILY_APP), but callable from the AppContainer at runtime —
// declared directly. winmm.lib is pulled in by the #pragma comment(lib) in
// src/trt/bridge_winmm.cpp.
extern "C" unsigned int __stdcall timeBeginPeriod(unsigned int uPeriod);
// TerminateThread is desktop-partition like timeBeginPeriod — declared
// directly (item 5: the force-kill watchdog's last resort).
extern "C" int __stdcall TerminateThread(void* hThread,
                                         unsigned long dwExitCode);
extern "C" void trt_gap_env_set(const char* name, const char* value);
// AddVectoredExceptionHandler is desktop-partition in the SDK headers, but
// present in kernel32.dll and callable from the AppContainer at runtime.
extern "C" void* __stdcall AddVectoredExceptionHandler(
    unsigned long First,
    LONG (WINAPI *Handler)(EXCEPTION_POINTERS*));

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <csetjmp>
#include <csignal>
#include <exception>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <new>
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
std::string g_package_games_dir;
std::string g_network_games_dir;
std::mutex g_status_mu;
std::string g_guest_status = "idle";
std::string g_steam_status = "press Y to link Steam (your own account)";
std::string g_steam_qr;  // URL to render as QR (steam or transfer)
std::atomic<bool> g_steam_busy{false};
std::atomic<bool> g_steam_needs_rescan{false};
std::atomic<bool> g_shutdown{false};
std::atomic<bool> g_guest_running{false};
DWORD g_ui_tid = 0;
std::thread g_guest_thread;
std::string g_running_game_name;  // guest currently running ("" when idle) — heartbeat + logs
ULONGLONG g_boot_ms = 0;          // GetTickCount64 at Run() start — heartbeat uptime
ULONGLONG g_last_rescan_ms = 0;   // GetTickCount64 of the last shelf rescan — debounce clock
std::string g_pending_launch;

// In-console autopilot state owned by the host loop (see host_autopilot.h).
// webxfer arms it on transfer; the launch handoff and its result drive the
// transitions; the repair/retry itself is applied by webxfer's state machine.
static longan::host_autopilot_state& g_host_ap = longan::host_ap_state();
bool g_mouse_mode = false;
bool g_kbd_mouse_mode = false;
// Set only when pad-as-mouse was chosen automatically because no pointer was
// visible at boot. A mouse connected later clears it so the real device takes
// over the cursor immediately (the mouse allowlist is present in our package
// identity, but the shell may attach the device after launch).
std::atomic<bool> g_auto_pad_mouse{false};
bool g_quick_menu_open = false;
std::string g_control_code_cached;
std::atomic<bool> g_f8_down{false};
std::atomic<bool> g_f8_pressed{false};
std::atomic<ULONGLONG> g_f8_edge_ms{0};
std::atomic<bool> g_km_keys[256]{};
bool g_booted = false;
bool g_setup_done = false;      // first-run setup screen flag
bool g_setup_active = false;    // currently showing the setup screen
// Adaptive Controller-vs-KBM mode: starts true on Xbox; switches false on any
// keyboard/mouse input and hides controller button prompts until gamepad input
// resumes.
std::atomic<bool> g_controller_active{true};
std::atomic<ULONGLONG> g_last_mouse_move_ms{0};
std::atomic<ULONGLONG> g_last_raw_mouse_ms{0};
// Last CoreWindow-derived pointer event (motion/press/wheel). The Win32
// GetCursorPos fallback yields while this is fresh so the two absolute
// sources never fight over the same frame.
std::atomic<ULONGLONG> g_last_core_pointer_ms{0};
// Whether the OS presents a hardware pointer to this package. Xbox withholds
// mouse input from titles that do not carry the publisher's opt-in, and the
// shell does not expose a way for an app to request it; the launcher needs to
// know this before it decides how a mouse-driven title will get a pointer.
std::atomic<bool> g_host_mouse_present{true};
std::wstring g_tool_result;
int g_del_confirm_idx = -1;
ULONGLONG g_del_confirm_ms = 0;

bool delete_dir_recursive_host(const std::string& path) {
  WIN32_FIND_DATAA fd{};
  HANDLE h = FindFirstFileA((path + "\\*").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
        continue;
      const std::string child = path + "\\" + fd.cFileName;
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        delete_dir_recursive_host(child);
      } else {
        SetFileAttributesA(child.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileA(child.c_str());
      }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
  }
  SetFileAttributesA(path.c_str(), FILE_ATTRIBUTE_NORMAL);
  return RemoveDirectoryA(path.c_str()) != 0;
}

// dashboard settings screen state (rows mirror longan::host_settings;
// the string/int buffers are edited here, applied via settings_store)
longan::ui_setting g_set_rows[24];
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

void request_guest_stop();

inline bool handle_host_cursor_key(u16 raw_vk, u16 scan, bool ext, bool down) {
  // F5 is the host's emergency exit. Handle it here rather than in the guest
  // key stream so it works identically through CoreWindow, the foreground
  // keyboard interceptor, and native GameInput, and so a guest cannot swallow
  // it. request_guest_stop() is the same clean WM_CLOSE/WM_QUIT path as Alt+F4.
  if (raw_vk == 0x74 /*VK_F5*/ && g_guest_running.load()) {
    if (down) request_guest_stop();
    return true;
  }
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
  // Only defer to MouseDevice while relative deltas arrived recently (<300ms);
  // never permanently lock out CoreWindow pointer motion on Xbox UWP.
  const ULONGLONG now = GetTickCount64();
  const ULONGLONG last_raw = g_last_raw_mouse_ms.load();
  if (g_uwp_raw_mouse.load() && last_raw != 0 && (now - last_raw) < 300) return;
  const auto bounds = window.Bounds();
  if (bounds.Width <= 0 || bounds.Height <= 0) return;
  int w, h;
  trt_display_size(&w, &h);
  if (w <= 0) w = 1920;
  if (h <= 0) h = 1080;
  const int x = std::clamp(static_cast<int>(point.X * w / bounds.Width), 0, w - 1);
  const int y = std::clamp(static_cast<int>(point.Y * h / bounds.Height), 0, h - 1);
  trt_input_pointer_move(x, y, false);
  g_last_mouse_move_ms.store(now);
  g_last_core_pointer_ms.store(now);
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
    bool ok = (XInputGetState(idx, &xs) == ERROR_SUCCESS);
    // Host cursor and launcher use physical pad state. Only the guest
    // receives keyboard-backed buttons, so they cannot feed back into KBM.
    if (idx == 0) {
      if (pad0_out) *pad0_out = xs;
      if (pad0_ok_out) *pad0_ok_out = ok;
    }
    // Games that expose only a controller prompt still need to be operable
    // when the user has a keyboard. Feed a small keyboard layout into the
    // guest virtual XInput slot while a game is running.
    if (idx == 0 && g_guest_running.load()) {
      const auto key = [](int vk) { return trt_key_down(vk); };
      const bool up = key(VK_UP), down = key(VK_DOWN);
      const bool left = key(VK_LEFT), right = key(VK_RIGHT);
      const bool accept = key(VK_RETURN) || key(VK_SPACE) || key('Z');
      const bool cancel = key(VK_ESCAPE) || key('C');
      const bool shoot = key('X');
      const bool lock = key(VK_SHIFT) || key('V');
      const bool wasd_left = key('A') && !key('D');
      const bool wasd_right = key('D') && !key('A');
      const bool wasd_up = key('W') && !key('S');
      const bool wasd_down = key('S') && !key('W');
      const bool keyboard_pad = up || down || left || right || accept ||
                                cancel || shoot || lock || wasd_left ||
                                wasd_right || wasd_up || wasd_down;
      // Advertise the keyboard-backed pad from startup so controller-aware
      // games enumerate it before the first key press.
      ok = true;
      if (keyboard_pad) {
        if (up) xs.Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
        if (down) xs.Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
        if (left) xs.Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
        if (right) xs.Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
        if (accept) xs.Gamepad.wButtons |= XINPUT_GAMEPAD_A;
        if (cancel) xs.Gamepad.wButtons |= XINPUT_GAMEPAD_B;
        if (shoot) xs.Gamepad.wButtons |= XINPUT_GAMEPAD_X;
        if (lock) xs.Gamepad.bLeftTrigger = 255;
        if (wasd_right) xs.Gamepad.sThumbLX = 32767;
        else if (wasd_left) xs.Gamepad.sThumbLX = -32768;
        if (wasd_up) xs.Gamepad.sThumbLY = 32767;
        else if (wasd_down) xs.Gamepad.sThumbLY = -32768;
        static unsigned keyboard_pad_trace = 0;
        if (keyboard_pad_trace++ < 16) {
          char line[144];
          snprintf(line, sizeof(line),
                   "input: keyboard-to-XInput buttons=%04x lx=%d ly=%d",
                   xs.Gamepad.wButtons, xs.Gamepad.sThumbLX,
                   xs.Gamepad.sThumbLY);
          longan::log_line(line);
        }
      }
    }
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
  std::string engine_name;
  std::string gfx_name;
  std::string audio_name;
  bool synthesized = false;
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
         s.find("setup") != std::string::npos ||   // installers, not games
         s.find("install") != std::string::npos ||
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
    const std::string rel_low = lower_ascii(rel);
    int score = 0;
    if (stem_low == g_low || stem_low == g_low + "-win64-shipping") score += 200;
    if (stem_low.size() > 15 &&
        stem_low.rfind("-win64-shipping") == stem_low.size() - 15) {
      score += 120;
    }
    if (stem_low.find("-win64-shipping") != std::string::npos ||
        stem_low.find("-shipping") != std::string::npos ||
        rel_low.find("binaries\\win64") != std::string::npos ||
        rel_low.find("binaries/win64") != std::string::npos ||
        rel_low.find("bin\\win64") != std::string::npos ||
        rel_low.find("bin/win64") != std::string::npos) {
      score += 2000;
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
  else if (machine == 0x014C) arch = "x86 32-bit (JIT)";
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
      else if (!_strnicmp(nm, "easyanticheat", 13) ||
               !_strnicmp(nm, "battleye", 8) ||
               !_strnicmp(nm, "beclient", 8)) mark = "[anti-cheat BLOCKED]";
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

// Scan ONE entry (a game folder, or a disc image dropped straight into
// the games folder) into g_games. Extracted from scan_games_root so a
// scoped rescan can rebuild just the changed game instead of re-walking
// every root + drive (item 4). `is_dir` comes from the FindFirstFile
// attributes on the sweep path; a scoped rescan passes the attributes
// it probed itself.
void scan_game_entry(
    const std::string& root, const std::string& n, bool is_dir,
    char drive_tag,
    const std::vector<std::pair<std::string, std::string>>& prev_choice) {
  if (n == "." || n == "..") return;
  if (!is_dir) {
    // A disc image dropped straight into the games folder: the exe list is
    // read out of the image and it mounts as D: on launch.
    const size_t dot = n.rfind('.');
    if (dot == std::string::npos) return;
    const std::string ext = lower_ascii(n.substr(dot));
    if (ext != ".iso" && ext != ".bin" && ext != ".img" && ext != ".cue") return;
    std::string img_path = root + "\\" + n;
    if (ext == ".cue") {
      // A .cue is a text index, not an image: mount the track it names.
      char track[1200] = {};
      if (!trt_iso_cue_track(img_path.c_str(), track, sizeof(track))) {
        char line[700];
        snprintf(line, sizeof(line),
                 "shelf: %s is a .cue whose FILE track could not be read "
                 "(skipped, named)",
                 img_path.c_str());
        longan::log_line(line);
        return;
      }
      char line[700];
      snprintf(line, sizeof(line), "shelf: %s -> track %s",
               (root + "\\" + n).c_str(), track);
      longan::log_line(line);
      img_path = track;
    }
    bool dup_img = false;
    for (const auto& existing : g_games)
      if (!_stricmp(existing.image.c_str(), img_path.c_str())) dup_img = true;
    if (dup_img) return;
    trt_iso* iso = trt_iso_open(img_path.c_str());
    if (!iso) {
      char line[600];
      snprintf(line, sizeof(line),
               "shelf: %s is not an ISO9660/UDF image (skipped, named)",
               img_path.c_str());
      longan::log_line(line);
      return;
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
      return;
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
    longan::cover_art_request(ge.name, ge.dir, 0);
    g_games.push_back(std::move(ge));
    return;
  }
  const std::string gdir = root + "\\" + n;
  if (GetFileAttributesA((gdir + longan::kSteamInstallPending).c_str()) !=
      INVALID_FILE_ATTRIBUTES) return;
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
  if (dup) return;
  std::vector<exe_cand> cands;
  collect_exes_rec(gdir, "", n, 0, &cands);
  // A folder that holds a disc image is a game too: the exe list comes from
  // inside the image and the image is mounted as D: at launch. The image wins
  // when the folder has no runnable exe, or when the image's best exe outscores
  // the folder's (a bundled installer or launcher must not beat the real game).
  std::string image_path;
  {
    char img[1024] = {};
    if (trt_iso_find_image_in_dir(gdir.c_str(), img, sizeof(img))) {
      std::vector<exe_cand> icands;
      trt_iso* iso = trt_iso_open(img);
      if (iso) {
        collect_iso_exes_rec(iso, "", n, 0, &icands);
        trt_iso_close(iso);
      }
      if (!icands.empty()) {
        std::sort(icands.begin(), icands.end(),
                  [](const exe_cand& a, const exe_cand& b) {
          if (a.score != b.score) return a.score > b.score;
          if (a.size != b.size) return a.size > b.size;
          return a.rel < b.rel;
        });
        int folder_score = -1;
        for (const auto& c : cands)
          if (c.score > folder_score) folder_score = c.score;
        if (icands[0].score > folder_score) {
          cands = icands;
          image_path = img;
          char line[800];
          snprintf(line, sizeof(line),
                   "shelf: %s mounts disc image %s (best exe inside: %s)",
                   n.c_str(), img, icands[0].rel.c_str());
          longan::log_line(line);
        } else {
          char line[800];
          snprintf(line, sizeof(line),
                   "shelf: %s runs from its folder exe over disc image %s "
                   "(the image is not mounted)",
                   n.c_str(), img);
          longan::log_line(line);
        }
      } else {
        char line[700];
        snprintf(line, sizeof(line),
                 "shelf: %s holds a disc image but no runnable exe inside it",
                 img);
        longan::log_line(line);
      }
    }
  }
  std::string steam_exe;
  if (image_path.empty()) {
    std::ifstream launch_file(gdir + longan::kSteamLaunchExe);
    if (std::getline(launch_file, steam_exe)) {
      if (!steam_exe.empty() && steam_exe.back() == '\r') steam_exe.pop_back();
      // A Steam entry can be deeper than the shelf scan or filtered as a helper.
      if (!steam_exe.empty() && std::none_of(cands.begin(), cands.end(), [&](const exe_cand& c) {
            return _stricmp(c.rel.c_str(), steam_exe.c_str()) == 0;
          })) {
        trt_engine_profile profile;
        if (trt_synthesize_engine(gdir.c_str(), steam_exe.c_str(), &profile)) {
          steam_exe = profile.main_exe_rel;
          const auto found = std::find_if(cands.begin(), cands.end(), [&](const exe_cand& c) {
            return _stricmp(c.rel.c_str(), steam_exe.c_str()) == 0;
          });
          if (found == cands.end()) {
            exe_cand candidate{};
            candidate.rel = steam_exe;
            cands.push_back(candidate);
          }
        } else {
          steam_exe.clear();
        }
      }
    }
  }
  if (cands.empty()) return;
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
  for (size_t k = 0; k < ge.exes.size() && !steam_exe.empty(); ++k) {
    if (_stricmp(ge.exes[k].c_str(), steam_exe.c_str()) == 0) {
      ge.exe_idx = static_cast<int>(k);
      break;
    }
  }
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
  // If not a disc image, run/load autonomous engine synthesis so the shelf
  // immediately displays the real engine and preferred graphics/audio layers.
  if (ge.image.empty() && !ge.dir.empty() && !ge.exe.empty()) {
    trt_engine_profile prof;
    if (trt_synthesize_engine(ge.dir.c_str(), ge.exe.c_str(), &prof)) {
      ge.engine_name = trt_engine_type_name(prof.engine_type);
      ge.gfx_name = trt_graphics_api_name(prof.preferred_gfx);
      ge.audio_name = trt_audio_middleware_name(prof.preferred_audio);
      ge.synthesized = true;
      if (!prof.main_exe_rel.empty() && prof.main_exe_rel != ge.exe) {
        for (size_t k = 0; k < ge.exes.size(); ++k) {
          if (!_stricmp(ge.exes[k].c_str(), prof.main_exe_rel.c_str())) {
            ge.exe_idx = (int)k;
            ge.exe = ge.exes[k];
            break;
          }
        }
      }
    }
  }
  {
    uint32_t appid = 0;
    std::ifstream sf(gdir + "\\steam_appid.txt");
    if (sf) {
      unsigned long v = 0;
      if (sf >> v) appid = static_cast<uint32_t>(v);
    }
    longan::cover_art_request(ge.name, ge.dir, appid);
  }
  g_games.push_back(std::move(ge));
}

void scan_games_root(
    const std::string& root, char drive_tag,
    const std::vector<std::pair<std::string, std::string>>& prev_choice) {
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA((root + "\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    scan_game_entry(root, fd.cFileName,
                    (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                    drive_tag, prev_choice);
  } while (FindNextFileA(h, &fd));
  FindClose(h);
}

// The exe the user had picked per game (keyed by dir or image path),
// so a rescan restores the selection instead of resetting to exe 0.
std::vector<std::pair<std::string, std::string>> snapshot_prev_choice() {
  std::vector<std::pair<std::string, std::string>> prev_choice;
  for (const auto& ge : g_games) {
    prev_choice.push_back({!ge.image.empty() ? ge.image
                            : (ge.dir.empty() ? ge.name : ge.dir),
                           ge.exe});
  }
  return prev_choice;
}

void refresh_storage_stats() {
  if (g_local_state.empty()) return;
  ULARGE_INTEGER avail{}, total{}, free_b{};
  if (GetDiskFreeSpaceExA(g_local_state.c_str(), &avail, &total, &free_b) &&
      total.QuadPart > 0) {
    const double total_gb = static_cast<double>(total.QuadPart) / 1e9;
    const double used_gb =
        static_cast<double>(total.QuadPart - free_b.QuadPart) / 1e9;
    longan::ui_set_storage(used_gb, total_gb);
  }
}

std::wstring infer_engine_label(const game_entry& ge) {
  const std::string c = lower_ascii(ge.compat);
  if (c.find("anti-cheat") != std::string::npos) return L"Anti-Cheat (Blocked)";
  if (ge.synthesized && !ge.engine_name.empty()) {
    std::string label = ge.engine_name;
    if (!ge.gfx_name.empty() && ge.gfx_name != "Direct3D 11") {
      label += " (" + ge.gfx_name + ")";
    }
    return u82ws(label);
  }
  if (c.find("32-bit") != std::string::npos) return L"x86 32-bit (JIT)";
  if (c.find("arm64") != std::string::npos) return L"ARM64 (Unsupported)";
  if (c.find("d3d12") != std::string::npos) return L"Direct3D 12";
  if (c.find("vulkan") != std::string::npos) return L"Vulkan -> D3D11";
  if (c.find("unity") != std::string::npos) return L"Unity Engine";
  if (c.find("d3d11") != std::string::npos) return L"Direct3D 11";
  if (c.find("opengl") != std::string::npos) return L"OpenGL -> D3D11";
  if (c.find("d3d9") != std::string::npos) return L"Direct3D 9";
  if (!ge.image.empty()) return L"Disc Image";
  return L"Windows x64";
}

void cycle_selected_exe(int idx, int delta) {
  if (idx < 0 || idx >= static_cast<int>(g_games.size())) return;
  auto& ge = g_games[idx];
  const int n = static_cast<int>(ge.exes.size());
  if (n <= 1) {
    longan::ui_toast(L"Only one executable in " + u82ws(ge.name), 2.2f);
    return;
  }
  ge.exe_idx = (ge.exe_idx + n + delta) % n;
  ge.exe = ge.exes[ge.exe_idx];
  if (ge.image.empty() && !ge.dir.empty()) {
    std::ofstream ef(ge.dir + "\\tiramisu_exe.txt", std::ios::binary);
    if (ef) ef << ge.exe << "\n";
  }
  char compat[280];
  if (!ge.image.empty()) {
    trt_iso* iso = trt_iso_open(ge.image.c_str());
    scan_compat_iso(iso, ge.exe, compat, (unsigned)sizeof(compat));
    trt_iso_close(iso);
  } else {
    scan_compat(ge.dir + "\\" + ge.exe, compat, (unsigned)sizeof(compat));
  }
  ge.compat = compat;
  longan::ui_toast(L"Switched exe: " + u82ws(ge.exe), 2.5f);
}

void cycle_selected_input(int idx) {
  if (idx < 0 || idx >= static_cast<int>(g_games.size())) return;
  auto& ge = g_games[idx];
  static const char* kModes[4] = {"auto", "controller", "kbm", "pad2kbm"};
  int cur = 0;
  for (int i = 0; i < 4; ++i) {
    if (ge.input_mode == kModes[i]) cur = i;
  }
  ge.input_mode = kModes[(cur + 1) % 4];
  if (ge.image.empty() && !ge.dir.empty()) {
    std::ofstream inf(ge.dir + "\\tiramisu_input.txt", std::ios::binary);
    if (inf) inf << ge.input_mode << "\n";
  }
  longan::ui_toast(u82ws(ge.name + " input -> " + ge.input_mode), 2.5f);
}

void sort_games_shelf() {
  // Hide synthetic test harnesses (d3d12triangle / guest_hello) from the real shelf
  g_games.erase(std::remove_if(g_games.begin(), g_games.end(),
                               [](const game_entry& ge) {
                                 const std::string low = lower_ascii(ge.name);
                                 return low == "d3d12triangle" ||
                                        low == "selftest" ||
                                        low == "guest_hello";
                               }),
                g_games.end());
  auto rank = [](const game_entry& ge) -> int {
    const std::string low = lower_ascii(ge.name);
    if (low == "celeste") return 0;
    if (low == "sekiro") return 1;
    if (low == "ultrapool") return 2;
    if (low.rfind("stage-", 0) == 0 || low.rfind("stage_", 0) == 0) return 4;
    return 3;
  };
  std::sort(g_games.begin(), g_games.end(),
            [&](const game_entry& a, const game_entry& b) {
              const int ra = rank(a), rb = rank(b);
              if (ra != rb) return ra < rb;
              return lower_ascii(a.name) < lower_ascii(b.name);
            });
}

std::vector<longan::webxfer_storage_volume> g_external_volumes;

std::string volume_key(std::string path) {
  while (path.size() > 1 && (path.back() == '\\' || path.back() == '/'))
    path.pop_back();
  return lower_ascii(std::move(path));
}

bool poll_external_storage(bool force = false) {
  using winrt::Windows::Foundation::AsyncStatus;
  using winrt::Windows::Foundation::IAsyncOperation;
  using winrt::Windows::Foundation::Collections::IVectorView;
  using winrt::Windows::Storage::KnownFolders;
  using winrt::Windows::Storage::StorageFolder;
  static IAsyncOperation<IVectorView<StorageFolder>> pending{nullptr};
  static std::vector<longan::webxfer_storage_volume> brokered;
  static std::string broker_status = "checking";
  static std::string published_status;
  static ULONGLONG next_probe = 0;

  const ULONGLONG now = GetTickCount64();
  if (pending && pending.Status() != AsyncStatus::Started) {
    try {
      std::vector<longan::webxfer_storage_volume> discovered;
      broker_status = "unavailable";
      if (pending.Status() == AsyncStatus::Completed) {
        for (const auto& folder : pending.GetResults()) {
          auto name = winrt::to_string(folder.Name());
          std::string path;
          try { path = winrt::to_string(folder.Path()); } catch (...) {}
          discovered.push_back({std::move(name), std::move(path), {}});
        }
        broker_status = "ready";
      }
      brokered = std::move(discovered);
    } catch (...) {
      broker_status = "unavailable";
      brokered.clear();
    }
    pending = nullptr;
    force = true;
    // Keep broker calls on the regular cadence when the async result lands.
    if (now >= next_probe) next_probe = now + 7000;
  } else if (!pending && (force || now >= next_probe)) {
    try {
      pending = KnownFolders::RemovableDevices().GetFoldersAsync();
    } catch (...) {
      broker_status = "unavailable";
      brokered.clear();
    }
  } else if (!force && now < next_probe) {
    return false;
  }
  if (now >= next_probe || force) next_probe = now + 7000;

  auto volumes = brokered;
  auto add_game_roots = [](longan::webxfer_storage_volume& volume) {
    if (volume.path.empty()) return;
    std::string base = volume.path;
    if (base.back() != '\\' && base.back() != '/') base += '\\';
    for (const char* child : {"Games", "TiramisuGames"}) {
      std::string path = base + child;
      const DWORD attr = GetFileAttributesA(path.c_str());
      if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
        continue;
      WIN32_FIND_DATAA file{};
      const HANDLE check = FindFirstFileA((path + "\\*").c_str(), &file);
      if (check != INVALID_HANDLE_VALUE) {
        FindClose(check);
      } else if (GetLastError() != ERROR_FILE_NOT_FOUND) {
        continue;
      }
      volume.game_roots.push_back(std::move(path));
    }
  };
  for (auto& volume : volumes) add_game_roots(volume);
  // Some consoles expose USB SSDs as fixed volumes. Only list fixed
  // drive-letter fallbacks when they expose an actual game folder.
  for (char letter = 'D'; letter <= 'Z'; ++letter) {
    const char root[4] = {letter, ':', '\\', 0};
    const UINT type = GetDriveTypeA(root);
    if (type != DRIVE_REMOVABLE && type != DRIVE_FIXED) continue;
    const auto key = volume_key(root);
    if (std::any_of(volumes.begin(), volumes.end(), [&](const auto& v) {
          return !v.path.empty() && volume_key(v.path) == key;
        })) continue;
    longan::webxfer_storage_volume volume{std::string(1, letter) + ":", root, {}};
    add_game_roots(volume);
    if (type == DRIVE_REMOVABLE || !volume.game_roots.empty())
      volumes.push_back(std::move(volume));
  }
  std::sort(volumes.begin(), volumes.end(), [](const auto& a, const auto& b) {
    if (a.path != b.path) return a.path < b.path;
    return a.name < b.name;
  });
  const bool changed = volumes != g_external_volumes;
  if (!changed && broker_status == published_status) return false;
  g_external_volumes = std::move(volumes);
  published_status = broker_status;
  longan::webxfer_set_external_storage(g_external_volumes, broker_status);
  longan::log_line(("storage: removable device discovery " + broker_status +
                   ", volumes=" + std::to_string(g_external_volumes.size())).c_str());
  return changed;
}

void rescan_games() {
  g_network_games_dir = longan::settings_get().network_games_root;
  std::vector<std::pair<std::string, std::string>> prev_choice =
      snapshot_prev_choice();
  g_games.clear();
  scan_games_root(g_games_dir, 0, prev_choice);
  if (!g_package_games_dir.empty() && g_package_games_dir != g_games_dir)
    scan_games_root(g_package_games_dir, 0, prev_choice);
  if (!g_network_games_dir.empty() && g_network_games_dir != g_games_dir &&
      g_network_games_dir != g_package_games_dir)
    scan_games_root(g_network_games_dir, 0, prev_choice);
  for (const auto& volume : g_external_volumes) {
    const char drive_tag = volume.path.size() > 1 && volume.path[1] == ':'
                               ? volume.path[0] : 0;
    for (const auto& root : volume.game_roots)
      scan_games_root(root, drive_tag, prev_choice);
  }
  sort_games_shelf();
  if (g_selected >= (int)g_games.size()) g_selected = 0;
  g_last_rescan_ms = (ULONGLONG)GetTickCount64();
  refresh_storage_stats();
}

// Full rescan of the games root only (no D:-Y: drive sweep): the
// periodic refresh and the debounce fallback. Drive sweeps are
// expensive (22 GetDriveType probes + walks) and removable drives
// rarely change mid-session; the full sweep still runs at boot and
// on a launch miss.
void rescan_games_root_only() {
  std::vector<std::pair<std::string, std::string>> prev_choice =
      snapshot_prev_choice();
  // Preserve removable-drive entries while refreshing all configured roots.
  const auto under_root = [](const std::string& path, const std::string& root) {
    return !root.empty() &&
           _strnicmp(path.c_str(), root.c_str(), root.size()) == 0;
  };
  std::vector<game_entry> external_games;
  for (auto& ge : g_games) {
    if (!under_root(ge.dir, g_games_dir) &&
        !under_root(ge.dir, g_package_games_dir) &&
        !under_root(ge.dir, g_network_games_dir)) {
      external_games.push_back(std::move(ge));
    }
  }
  g_games.clear();
  scan_games_root(g_games_dir, 0, prev_choice);
  if (!g_package_games_dir.empty() && g_package_games_dir != g_games_dir)
    scan_games_root(g_package_games_dir, 0, prev_choice);
  if (!g_network_games_dir.empty() && g_network_games_dir != g_games_dir &&
      g_network_games_dir != g_package_games_dir)
    scan_games_root(g_network_games_dir, 0, prev_choice);
  for (auto& eg : external_games) g_games.push_back(std::move(eg));
  sort_games_shelf();
  if (g_selected >= (int)g_games.size()) g_selected = 0;
  g_last_rescan_ms = (ULONGLONG)GetTickCount64();
  refresh_storage_stats();
}

// Scoped rescan (item 4): rebuild only the entries for one game dir.
// Drops that game's current entries first, then re-scans just its dir
// (plus a same-stem disc image in the games root, if one exists). A
// missing dir (deleted game) means the removal stands — the entries
// were already dropped and nothing is re-added.
void rescan_one_game(const std::string& name) {
  const std::string gdir = g_games_dir + "\\" + name;
  std::vector<std::pair<std::string, std::string>> prev_choice =
      snapshot_prev_choice();
  g_games.erase(std::remove_if(g_games.begin(), g_games.end(),
                               [&](const game_entry& ge) {
                                 if (!_stricmp(ge.dir.c_str(), gdir.c_str()))
                                   return true;
                                 // a same-stem disc image in the games root
                                 if (!ge.image.empty() &&
                                     _strnicmp(ge.image.c_str(), gdir.c_str(),
                                               gdir.size()) == 0) {
                                   const std::string tail =
                                       lower_ascii(ge.image.substr(gdir.size()));
                                   return tail == ".iso" || tail == ".bin" ||
                                          tail == ".img";
                                 }
                                 return false;
                               }),
                g_games.end());
  DWORD attrs = GetFileAttributesA(gdir.c_str());
  if (attrs != INVALID_FILE_ATTRIBUTES)
    scan_game_entry(g_games_dir, name,
                    (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0, 0, prev_choice);
  for (const char* ext : {".iso", ".bin", ".img", ".cue"}) {
    const std::string img = gdir + ext;
    DWORD ia = GetFileAttributesA(img.c_str());
    if (ia != INVALID_FILE_ATTRIBUTES &&
        !(ia & FILE_ATTRIBUTE_DIRECTORY))
      scan_game_entry(g_games_dir, name + ext, false, 0, prev_choice);
  }
  sort_games_shelf();
  if (g_selected >= (int)g_games.size()) g_selected = 0;
  g_last_rescan_ms = (ULONGLONG)GetTickCount64();
  refresh_storage_stats();
}

extern "C" int trt_mem_budget_charge_va(u64 bytes);
extern "C" void trt_mem_budget_discharge_va(u64 bytes);
extern "C" void trt_mem_budget_reset(void);


extern "C" void trt_mem_budget_report(char* line, unsigned cap);
// .223 DIAG (snapshot-only): appmem feed for VIDEO-SAMPLE.
extern "C" void trt_video_set_app_mem_mb(u64 mb);

// Brown-line diagnostics: report guest/app memory at boot stages.
void log_guest_memory(const char* stage) {
  constexpr u64 mib = 1024ULL * 1024ULL;
  char budget[192];
  char line[320];
  trt_mem_budget_report(budget, sizeof(budget));
  try {
    using winrt::Windows::System::MemoryManager;
    const u64 usage = MemoryManager::AppMemoryUsage();
    const u64 limit = MemoryManager::AppMemoryUsageLimit();
    // .223 DIAG (snapshot-only): publish the app working set
    // for the VIDEO-SAMPLE appmem field (one WinRT call
    // shared with this line, not a second ~1s call inside
    // the sample).
    trt_video_set_app_mem_mb(usage / mib);
    snprintf(line, sizeof(line), "guest memory (%s): app %llu/%llu MiB, %s",
             stage, (unsigned long long)(usage / mib),
             (unsigned long long)(limit / mib), budget);
  } catch (const winrt::hresult_error& ex) {
    snprintf(line, sizeof(line), "guest memory (%s): app usage query 0x%08lx, %s",
             stage, (unsigned long)ex.code().value, budget);
  }
  longan::log_line(line);
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
std::atomic<longan::launch_phase> g_launch_phase{longan::launch_phase::none};
std::atomic<longan::launch_outcome> g_launch_outcome{longan::launch_outcome::none};

void checkpoint_phase(longan::launch_phase phase) {
  g_launch_phase.store(phase);
  g_launch_outcome.store(longan::launch_outcome::running);
  longan::launch_checkpoint_write(g_local_state, {phase, longan::launch_outcome::running, 0});
}

void checkpoint_outcome(longan::launch_outcome outcome, uint32_t code) {
  const auto phase = g_launch_phase.load();
  auto expected = longan::launch_outcome::running;
  if (phase != longan::launch_phase::none &&
      g_launch_outcome.compare_exchange_strong(expected, outcome)) {
    longan::launch_checkpoint_write(g_local_state, {phase, outcome, code});
  }
}
std::atomic<bool> g_guest_stop_req{false};
std::atomic<u64> g_guest_stop_since{0};

extern "C" {
void trt_bridge_post_quit(int code);
void trt_real_exit_hook_install(void);
void trt_syscall_trap_install(void);
void trt_video_sample_log(void);
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

// Force-kill watchdog (item 5): last resort for a guest thread that ignores
// a stop request (wedged in a syscall the WM_CLOSE/WM_QUIT trap never
// reaches — the trap only fires when the guest pumps via host_pump) or that
// never returns from run_game. TerminateThread is a blunt instrument: the
// killed thread may have held a trt mutex (leaving it locked — the next
// launch may hit it) and its guest allocations leak. What it must NOT do is
// corrupt HOST state, so the teardown is explicit and ordered:
//   1. terminate the thread, then join it (the handle is ours until join;
//      the lambda's tail never runs for a killed thread, so the steps
//      below do the cleanup the tail would have done)
//   2. clear the running flag + running-game name (the UI must not think a
//      guest is still running)
//   3. clear the stop request (it was for the dead run)
//   4. reset the memory budget (the dead guest's charges are unrecoverable
//      any other way — the ledger would otherwise refuse the next launch)
static void apply_bridge_launch_settings();

void force_kill_guest(const char* reason) {
  if (!g_guest_thread.joinable()) return;
  char line[192];
  snprintf(line, sizeof(line),
           "host: FORCE-KILL guest thread (%s) — TerminateThread, budget reset",
           reason ? reason : "?");
  longan::log_line(line);
  ::TerminateThread(g_guest_thread.native_handle(), 1);
  g_guest_thread.join();
  longan::settings_clear_profile();
  apply_bridge_launch_settings();
  g_guest_running.store(false);
  longan::webxfer_set_running_game("");
  g_running_game_name.clear();
  g_guest_stop_req.store(false);
  trt_mem_budget_reset();
  longan::log_line("host: guest force-killed; budget reset, shelf live");
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
  // .213 DIAG: canary -- proves the filter was entered. If the filter itself
  // faults before logging, this line never appears (double fault in the filter).
  if (ep && ep->ExceptionRecord) {
    char canary[128];
    snprintf(canary, sizeof(canary), "guest_seh_filter entered code=0x%08lx",
             (unsigned long)ep->ExceptionRecord->ExceptionCode);
    longan::log_line(canary);
  } else {
    longan::log_line("guest_seh_filter entered (null record)");
  }
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
  // GPR + faulting-instruction dump (mirrors the old FATAL VEH format).
  // Must run BEFORE capture_fault_stack: the unwind mutates the context.
  if (ep->ContextRecord) {
    const CONTEXT& c = *ep->ContextRecord;
    char g[512];
    snprintf(g, sizeof(g),
             "  GPRs: rip=%llx rsp=%llx rbp=%llx rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx",
             (unsigned long long)c.Rip, (unsigned long long)c.Rsp, (unsigned long long)c.Rbp,
             (unsigned long long)c.Rax, (unsigned long long)c.Rbx, (unsigned long long)c.Rcx,
             (unsigned long long)c.Rdx, (unsigned long long)c.Rsi, (unsigned long long)c.Rdi);
    longan::log_line(g);
    snprintf(g, sizeof(g),
             "  GPRs2: r8=%llx r9=%llx r10=%llx r11=%llx r12=%llx r13=%llx r14=%llx r15=%llx",
             (unsigned long long)c.R8, (unsigned long long)c.R9, (unsigned long long)c.R10,
             (unsigned long long)c.R11, (unsigned long long)c.R12, (unsigned long long)c.R13,
             (unsigned long long)c.R14, (unsigned long long)c.R15);
    longan::log_line(g);
    unsigned char code[32];
    memset(code, 0, sizeof(code));
    bool got_code = false;
    __try {
      memcpy(code, record.ExceptionAddress, sizeof(code));
      got_code = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      got_code = false;
    }
    if (got_code) {
      char cb[256];
      char* p = cb;
      p += snprintf(p, sizeof(cb) - (size_t)(p - cb), "  code [pc..pc+32]:");
      for (int i = 0; i < 32; ++i)
        p += snprintf(p, sizeof(cb) - (size_t)(p - cb), " %02x", code[i]);
      longan::log_line(cb);
    }
  }
  // Crash bundle. On the console there is no debugger to attach and the log
  // tail is the only thing that survives a reboot, so the settings toggle
  // writes one file containing everything a post-mortem needs: the fault, the
  // registers, the settings that were in force, what shaders the title had
  // translated, and the log tail. Runs before the unwind below, because
  // capture_fault_stack mutates the context we want to record.
  if (longan::trt_settings_crash_report()) {
    longan::crash_report_input rep;
    char why[352];
    snprintf(why, sizeof(why), "guest fault: 0x%08lx at %s",
             (unsigned long)record.ExceptionCode, where);
    rep.reason = why;
    if (ep->ContextRecord) {
      const CONTEXT& c = *ep->ContextRecord;
      char d[512];
      snprintf(d, sizeof(d),
               "rip=%llx rsp=%llx rbp=%llx rax=%llx\n"
               "rbx=%llx rcx=%llx rdx=%llx rsi=%llx\n"
               "rdi=%llx r8=%llx r9=%llx r10=%llx\n"
               "r11=%llx r12=%llx r13=%llx r14=%llx r15=%llx",
               (unsigned long long)c.Rip, (unsigned long long)c.Rsp,
               (unsigned long long)c.Rbp, (unsigned long long)c.Rax,
               (unsigned long long)c.Rbx, (unsigned long long)c.Rcx,
               (unsigned long long)c.Rdx, (unsigned long long)c.Rsi,
               (unsigned long long)c.Rdi, (unsigned long long)c.R8,
               (unsigned long long)c.R9, (unsigned long long)c.R10,
               (unsigned long long)c.R11, (unsigned long long)c.R12,
               (unsigned long long)c.R13, (unsigned long long)c.R14,
               (unsigned long long)c.R15);
      rep.detail = d;
    }
    rep.settings_ini = longan::settings_get().to_ini();
    rep.census_json = spirv_census_json();
    char tail[8192];
    tail[0] = 0;
    longan::log_get_recent(tail, sizeof(tail));
    rep.log_tail = tail;
    const std::string rep_path =
        longan::crash_report_write(g_local_state, rep);
    if (!rep_path.empty()) {
      char note[320];
      snprintf(note, sizeof(note), "crash report: %s", rep_path.c_str());
      longan::log_line(note);
    } else {
      longan::log_line("crash report: write failed (see LocalState\\crash)");
    }
  }
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
    if (img->tls_callback_count) {
      checkpoint_phase(longan::launch_phase::native_tls);
      for (int i = 0; i < img->tls_callback_count; ++i) {
        trt_call_guest_img((void*)(img->base + img->tls_callback_rvas[i]), img,
                           (u64)(uintptr_t)img->base, 1, 0, 0);
      }
    }
    checkpoint_phase(longan::launch_phase::native_entry);
    *out_rc = trt_call_guest_img(
        (void*)(img->base + img->entry_rva), img,
        (u64)(uintptr_t)img->base, 0, 0, 0);
    return 0;
  } __except (guest_seh_filter(GetExceptionInformation())) {
    *out_ex = GetExceptionCode();
    checkpoint_outcome(longan::launch_outcome::guest_fault, *out_ex);
    return 1;
  }
}

void run_guest_trapped(trt_image* img, char* detail, size_t cap) {
  // .216 DIAG (snapshot-only): arm the real-exit INT3 hook before any
  // guest code runs, so a PEB-walked / direct real ExitProcess is caught.
  trt_real_exit_hook_install();
  // .220 DIAG (snapshot-only): arm the syscall trap AT LAUNCH, not
  // from the gap final dump -- the .219 trap never armed because
  // the final dump needs 2s of wall-time quiet and the guest dies
  // first. The trap re-arms silently on the watchdog tick after
  // host trips; guest fires re-arm up to 3 times per target.
  trt_syscall_trap_install();
  g_exit_code = 0;
  g_raise_code = 0;
  g_guest_tid = GetCurrentThreadId();
  trt_boot_exit_trap = &host_exit_trap;
  trt_boot_raise_trap = nullptr;  // let caught C++ EH unwind via OS .pdata
  // .212 DIAG: host watchdog -- log total app memory + guest ledger every 1s
  // while a guest runs. If the platform kills the app (Game-package memory
  // ceiling), the last watchdog lines show the trajectory right up to death
  // (log_line flushes, so they survive). log_guest_memory reports both the
  // platform AppMemoryUsage and the bridge ledger commit.
  std::atomic<bool> wd_stop{false};
  std::thread wd_thread([&wd_stop] {
    try {
      winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
    }
    // .213 DIAG: watchdog starting -- proves the thread launched.
    longan::log_line("watchdog starting");
    // .220 DIAG (snapshot-only): watchdog-block instrumentation --
    // the watchdog ticks at ~1Hz, not 10ms, while the process is
    // idle (guest 843ms CPU total in .219), so something BLOCKS
    // it. Time each step (mem query / CPU feed / gap dump) and
    // the inter-tick gap; a step >50ms or a gap >100ms logs with
    // the breakdown, naming the blocker (prime suspect: the
    // MemoryManager::AppMemoryUsage() WinRT call inside
    // log_guest_memory, ~1s per call).
    auto wd_tick_prev = std::chrono::steady_clock::now();
    int wd_tick_n = 0;
    while (!wd_stop.load()) {
      const auto wd_tick_start = std::chrono::steady_clock::now();
      const long long wd_tick_gap_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              wd_tick_start - wd_tick_prev)
              .count();
      wd_tick_prev = wd_tick_start;
      ++wd_tick_n;
      try {
        const auto wd_t0 = std::chrono::steady_clock::now();
        log_guest_memory("watchdog");
        const auto wd_t1 = std::chrono::steady_clock::now();
        // .219 DIAG (snapshot-only): guest-CPU proof -- feed the guest
        // main-thread CPU time + whole-process CPU time into the gap
        // trace each tick; the dump prints them, so advancing CPU with
        // zero new bridged calls proves the guest is SPINNING (vs
        // dead/blocked). g_guest_thread is the guest main thread.
        if (g_guest_thread.joinable()) {
          FILETIME c, e, k, u;
          if (::GetThreadTimes(g_guest_thread.native_handle(), &c, &e, &k,
                               &u)) {
            const u64 guest_ms =
                ((((u64)k.dwHighDateTime << 32) | (u64)k.dwLowDateTime) +
                 (((u64)u.dwHighDateTime << 32) | (u64)u.dwLowDateTime)) /
                10000ull;
            FILETIME pc, pe, pk, pu;
            u64 proc_ms = 0;
            if (::GetProcessTimes(::GetCurrentProcess(), &pc, &pe, &pk,
                                  &pu)) {
              proc_ms = ((((u64)pk.dwHighDateTime << 32) |
                          (u64)pk.dwLowDateTime) +
                         (((u64)pu.dwHighDateTime << 32) |
                          (u64)pu.dwLowDateTime)) /
                        10000ull;
            }
            gap_trace_set_guest_cpu_ms(guest_ms, proc_ms);
          }
        }
        const auto wd_t2 = std::chrono::steady_clock::now();
        // .218 DIAG (snapshot-only): dump the gap API-call trace when it
        // changed (at most once per second wall-clock + a final dump
        // 2s of wall time after the last call, so the final calls
        // before a silent death are logged).
        gap_trace_dump_if_changed();
        const auto wd_t3 = std::chrono::steady_clock::now();
        const long long wd_mem_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(wd_t1 -
                                                                  wd_t0)
                .count();
        const long long wd_cpu_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(wd_t2 -
                                                                  wd_t1)
                .count();
        const long long wd_gap_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(wd_t3 -
                                                                  wd_t2)
                .count();
        // .220 DIAG (snapshot-only): the blocker verdict -- a slow
        // step names the blocking call; a big gap with fast steps
        // means the sleep/scheduler never woke the thread.
        if (wd_mem_ms > 50 || wd_cpu_ms > 50 || wd_gap_ms > 50 ||
            wd_tick_gap_ms > 100) {
          char wl[256];
          snprintf(wl, sizeof(wl),
                   "watchdog: tick #%d gap=%llums steps mem=%llums cpu=%llums gapdump=%llums",
                   wd_tick_n, wd_tick_gap_ms, wd_mem_ms, wd_cpu_ms, wd_gap_ms);
          longan::log_line(wl);
        }
        // .221 DIAG (snapshot-only): periodic step-time sample
        // -- the .220 verdict only logs SLOW ticks, so the
        // steady ~100-150ms per-tick cost is invisible.
        // Sample every 10th tick.
        if ((wd_tick_n % 10) == 0) {
          char sl[256];
          snprintf(sl, sizeof(sl),
                   "watchdog: step-sample tick #%d gap=%llums steps mem=%llums cpu=%llums gapdump=%llums",
                   wd_tick_n, wd_tick_gap_ms, wd_mem_ms, wd_cpu_ms, wd_gap_ms);
          longan::log_line(sl);
        }
        // .220 DIAG (snapshot-only): VIDEO-SAMPLE every ~5s wall
        // time -- growth vs baseline for the 3809MB video usage.
        static auto wd_last_video = std::chrono::steady_clock::now();
        if (wd_tick_start - wd_last_video >= std::chrono::seconds(5)) {
          wd_last_video = wd_tick_start;
          trt_video_sample_log();
        }
      } catch (...) {
        // .213 DIAG: tick exception -- watchdog survived a bad tick.
        longan::log_line("watchdog: tick exception");
      }
      // .214 DIAG: 10ms cadence -- the .213 250ms cadence was slower than
      // the final death phase, so a missing tick could not prove the
      // watchdog died. At 10ms a missing tick during death is decisive.
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  });
  // .221 DIAG (snapshot-only): second-thread heartbeat -- a
  // separate lightweight thread logging every second. If the
  // heartbeat stops but the watchdog ticks, the watchdog is
  // suspended; if both stop, the process is suspended/dying.
  // This separates watchdog-suspension from process-suspension
  // at death (the .220 blocker question).
  std::thread hb_thread([&wd_stop] {
    int hb_n = 0;
    while (!wd_stop.load()) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      if (wd_stop.load()) break;
      ++hb_n;
      char hl[64];
      snprintf(hl, sizeof(hl), "watchdog: heartbeat #%d", hb_n);
      longan::log_line(hl);
    }
  });
  const int trapped = setjmp(g_exit_jmp);
  if (trapped == 0) {
    u64 rc = 0;
    DWORD ex_code = 0;
    if (call_guest_seh(img, &rc, &ex_code) == 0) {
      checkpoint_outcome(longan::launch_outcome::returned, static_cast<uint32_t>(rc));
      snprintf(detail, cap, "guest returned %llu", (unsigned long long)rc);
    } else {
      snprintf(detail, cap, "guest unhandled exception 0x%08lx",
               (unsigned long)ex_code);
    }
  } else if (trapped == 1) {
    checkpoint_outcome(longan::launch_outcome::guest_exit, static_cast<uint32_t>(g_exit_code));
    snprintf(detail, cap, "guest ExitProcess(%d)", g_exit_code);
  } else {
    checkpoint_outcome(longan::launch_outcome::guest_fault, g_raise_code);
    snprintf(detail, cap, "guest raised exception 0x%08x", g_raise_code);
  }
  trt_boot_exit_trap = nullptr;
  trt_boot_raise_trap = nullptr;
  g_guest_tid = 0;
  wd_stop.store(true);
  if (wd_thread.joinable()) wd_thread.join();
  if (hb_thread.joinable()) hb_thread.join();
}

// Settings the bridge layers read once per launch. Pushed rather than pulled
// so the runtime keeps its weak-symbol contract with offline test binaries.
static void apply_bridge_launch_settings() {
  // FSB5 console decode gate: off unless the user (or a per-title profile)
  // opts in, because the unhardened decode corrupted the heap on a Series S.
  // Before this call existed the gate documented "the host calls this with the
  // fsb_decode setting" and nothing ever did.
  trt_fsb5_set_console_decode(longan::trt_settings_fsb_decode() ? 1 : 0);

  const int configured_mb = longan::trt_settings_memory_budget_mb();
  if (configured_mb > 0) {
    auto budget_mb = static_cast<unsigned>(configured_mb);
    try {
      using winrt::Windows::System::MemoryManager;
      const auto usage = MemoryManager::AppMemoryUsage();
      const auto limit = MemoryManager::AppMemoryUsageLimit();
      budget_mb = longan::console_guest_memory_budget_mb(budget_mb, usage, limit);
    } catch (const winrt::hresult_error&) {
      // A console OS that cannot query the cap still uses its configured budget.
    }
    trt_mem_budget_set_mb(budget_mb);
  }
}

guest_run_result run_game(const std::string& name, const std::string& exe,
                          const std::string& game_dir,
                          const std::string& image = std::string()) {
  struct launch_profile_scope {
    ~launch_profile_scope() {
      longan::settings_clear_profile();
      apply_bridge_launch_settings();
    }
  } profile_scope;
  guest_run_result r{};
  struct checkpoint_scope {
    const guest_run_result& result;
    ~checkpoint_scope() {
      checkpoint_outcome(longan::launch_outcome::returned, static_cast<uint32_t>(result.map_rc));
      g_launch_phase.store(longan::launch_phase::none);
    }
  } checkpoint{r};
  checkpoint_phase(longan::launch_phase::preparing);
  char line[256];

  trt_gpu_reset_render_stats();
  longan::swapchain_reset_guest_draw_probe();
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
  // Universal save directories: provision AppData\LocalLow, AppData\Local,
  // AppData\Roaming, Saved Games, and Documents\My Games for the title.
  if (!name.empty()) {
    std::string safe_name = name;
    for (char& c : safe_name) {
      if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
        c = '_';
      }
    }
    trt_vfs_mkdir_p((drive_c + "\\Users\\Player\\AppData\\LocalLow\\" + safe_name).c_str());
    trt_vfs_mkdir_p((drive_c + "\\Users\\Player\\AppData\\Local\\" + safe_name).c_str());
    trt_vfs_mkdir_p((drive_c + "\\Users\\Player\\AppData\\Roaming\\" + safe_name).c_str());
    trt_vfs_mkdir_p((drive_c + "\\Users\\Player\\Saved Games\\" + safe_name).c_str());
    trt_vfs_mkdir_p((drive_c + "\\Users\\Player\\Documents\\My Games\\" + safe_name).c_str());
  }
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
  checkpoint_phase(longan::launch_phase::mounted);
  // A game can ship its own defaults. The writable profile for this source
  // layers user overrides on top, including for read-only games and discs.
  {
    const auto titles_dir = longan::settings_title_profile_dir(
        g_local_state, image.empty() ? game_dir : image);
    const int stub = longan::settings_write_profile_stub(titles_dir, name);
    const int prof = longan::settings_load_profile(game_dir, titles_dir);
    // A per-title profile may flip fsb_decode or the quality tier, so the
    // bridge knobs are re-pushed after the profile lands.
    apply_bridge_launch_settings();
    char pline[600];
    snprintf(pline, sizeof(pline),
             "profile: %d keys applied for %s (starter %s)", prof,
             name.c_str(),
             stub == 1 ? "written to LocalState\\profiles"
                       : (stub == 0 ? "already present" : "not writable"));
    longan::log_line(pline);
    // A profile that names a service we cannot provide should say so here,
    // not surface later as a game that mysteriously fails to initialise.
    const std::string& need = longan::settings_get().required_services;
    if (!need.empty()) {
      std::string unknown;
      const int bad = longan::settings_unknown_services(need, &unknown);
      if (bad) {
        snprintf(pline, sizeof(pline),
                 "profile: %d unrecognised required service(s): %s", bad,
                 unknown.c_str());
      } else {
        snprintf(pline, sizeof(pline), "profile: requires %s (all known)",
                 need.c_str());
      }
      longan::log_line(pline);
    }
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
    const size_t rel_sl = exe.find_last_of("/\\");
    const std::string guest_cwd =
        (rel_sl == std::string::npos) ? guest_cwd_root
                                      : (guest_cwd_root + exe.substr(0, rel_sl));
    trt_bridge_set_cwd(guest_cwd.c_str());
  }
  // A game-shipped tiramisu_args.txt, or launch_args from the per-title
  // profile, both win over the synthesized arguments.
  bool has_user_args = false;
  bool app_id_read = false;     // a non-zero steam_appid.txt was found
  {
    std::ifstream af(game_dir + "\\tiramisu_args.txt");
    if (af) {
      has_user_args = true;
      std::string args, ln;
      while (std::getline(af, ln)) {
        if (!ln.empty() && ln.back() == '\r') ln.pop_back();
        if (ln.empty() || ln[0] == '#') continue;
        if (!args.empty()) args += ' ';
        args += ln;
      }
      trt_bridge_set_cmd_args(args.c_str());
    } else {
      const std::string prof_args = longan::settings_get().launch_args;
      if (!prof_args.empty()) {
        trt_bridge_set_cmd_args(prof_args.c_str());
        has_user_args = true;  // tuned for this title, keep the synthesizer out
        longan::log_line(
            ("host: args from per-title profile: " + prof_args).c_str());
      } else {
        trt_bridge_set_cmd_args("");
      }
    }
    const u32 app_id = trt_steam_read_appid(game_dir.c_str());
    app_id_read = (app_id != 0);
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
    }
    char hexe[1400] = {};
    const std::wstring wexe = u82ws(guest_exe);
    if (trt_vfs_image_materialize((const u16*)wexe.c_str(), hexe,
                                  sizeof(hexe), nullptr) != 0) {
      r.map_rc = TRT_ERR_BAD_PE;
      snprintf(r.detail, sizeof(r.detail),
               "Could not extract %s from the mounted disc", exe.c_str());
      return r;
    }
    exe_path = hexe;
  }
  trt_engine_profile synth_prof;
  const bool synth_ok = image_mounted
      ? trt_synthesize_engine_for_file(exe_path.c_str(), &synth_prof)
      : trt_synthesize_engine(game_dir.c_str(), exe.c_str(), &synth_prof);
  if (!synth_ok) {
    r.map_rc = TRT_ERR_BAD_PE;
    snprintf(r.detail, sizeof(r.detail),
             "No supported Windows executable found for %s", exe.c_str());
    return r;
  }

  // Unity's native launcher loads the game's embedded Mono runtime after the
  // profile is synthesized. mono_host.cpp configures the separate managed-EXE
  // path; expose the same supported SGen settings through the guest environment
  // bridge before UnityPlayer/mono.dll can read them.
  if (synth_prof.engine_type == TRT_ENGINE_UNITY_MONO) {
    constexpr const char* gc_params = "nursery-size=64m,major=marksweep";
    trt_gap_env_set("MONO_GC_PARAMS", gc_params);
    trt_gap_env_set("MONO_LOG_MASK", "gc");
    trt_gap_env_set("MONO_LOG_LEVEL", "debug");
    SetEnvironmentVariableW(L"MONO_GC_PARAMS", L"nursery-size=64m,major=marksweep");
    SetEnvironmentVariableW(L"MONO_LOG_MASK", L"gc");
    SetEnvironmentVariableW(L"MONO_LOG_LEVEL", L"debug");
    longan::log_line("host: Unity Mono guest env MONO_GC_PARAMS=nursery-size=64m,major=marksweep");
  }

  std::string effective_exe = exe;
  // If the synthesizer selected an optimal executable in a subfolder (e.g. Binaries\Win64),
  // update exe_path and guest working paths so the real game binary and its sibling DLLs run.
  if (!image_mounted && !synth_prof.main_exe_abs.empty() && synth_prof.main_exe_abs != exe_path) {
    DWORD attrs = GetFileAttributesA(synth_prof.main_exe_abs.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
      char ch_msg[512];
      snprintf(ch_msg, sizeof(ch_msg),
               "host: [ENGINE SYNTHESIZER] redirected executable: %s -> %s",
               exe_path.c_str(), synth_prof.main_exe_abs.c_str());
      longan::log_line(ch_msg);
      exe_path = synth_prof.main_exe_abs;
      std::string target_exe = synth_prof.main_exe_rel;
      effective_exe = target_exe;
      guest_exe = "Z:\\" + target_exe;
      trt_bridge_set_exe_path(guest_exe.c_str());
      const size_t rel_sl = target_exe.find_last_of("/\\");
      const std::string guest_cwd =
          (rel_sl == std::string::npos) ? guest_cwd_root
                                        : (guest_cwd_root + target_exe.substr(0, rel_sl));
      trt_bridge_set_cwd(guest_cwd.c_str());
    }
  }

  std::string steam_args;
  if (!image_mounted) {
    std::ifstream launch_file(game_dir + longan::kSteamLaunchExe);
    std::string launch_exe;
    std::string working_dir;
    if (std::getline(launch_file, launch_exe) &&
        _stricmp(launch_exe.c_str(), effective_exe.c_str()) == 0) {
      std::getline(launch_file, working_dir);
      std::getline(launch_file, steam_args);
      std::string normalized;
      if (working_dir == ".") {
        trt_bridge_set_cwd(guest_cwd_root.c_str());
      } else if (trt_steam_normalize_content_path(working_dir, &normalized)) {
        const DWORD attrs = GetFileAttributesA((game_dir + "\\" + normalized).c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY))
          trt_bridge_set_cwd((guest_cwd_root + normalized).c_str());
      }
    }
  }

  char synth_msg[512];
  snprintf(synth_msg, sizeof(synth_msg),
           "host: [ENGINE SYNTHESIZER] %s engine=%s arch=%s gfx=%s audio=%s universal_flags=%u",
           synth_prof.game_name.c_str(),
           trt_engine_type_name(synth_prof.engine_type),
           synth_prof.is_managed ? "Managed (.NET)" : (synth_prof.is_pe32 ? "PE32" : "PE64"),
           trt_graphics_api_name(synth_prof.preferred_gfx),
           trt_audio_middleware_name(synth_prof.preferred_audio),
           synth_prof.universal_flags);
  longan::log_line(synth_msg);
  if (synth_prof.engine_type == TRT_ENGINE_UNIVERSAL) {
    // Universal: no engine evidence. What actually happens is the native guest
    // path plus blanket stub provisioning for unknown imports (see
    // trt_autonomous_provision_runtime); PC imports are translated, not
    // recompiled, so say that instead of claiming a port that does not exist.
    longan::log_line("host: universal engine -> native guest path with translated "
                     "imports and blanket stubs (no engine-specific args applied)");
  }
  // Keep explicit title settings first, then the Steam launch command,
  // and use the generated engine defaults when neither specifies arguments.
  if (!has_user_args) {
    std::string launch_args = std::move(steam_args);
    if (launch_args.empty()) {
      for (const auto& arg : synth_prof.cmdline_args) {
        if (!launch_args.empty()) launch_args += ' ';
        launch_args += arg;
      }
    }
    if (!launch_args.empty()) {
      trt_bridge_set_cmd_args(launch_args.c_str());
      longan::log_line(("host: launch arguments: " + launch_args).c_str());
    }
  }
  if (!app_id_read && synth_prof.steam_appid) {
    trt_steam_set_appid(synth_prof.steam_appid);
    char al[160];
    snprintf(al, sizeof(al),
             "host: [ENGINE SYNTHESIZER] steam appid %u applied",
             synth_prof.steam_appid);
    longan::log_line(al);
  }
  // Engine-owned screen settings: a title that ignores -ResX/-screen-width still
  // reads its own settings file, and its shipped default is usually the PC
  // 4K/12 GB one that cannot fit the Dev-Mode budget.
  {
    char up[1200] = {};
    if (trt_engine_write_user_settings(&synth_prof, drive_c.c_str(), up, sizeof(up))) {
      std::string ul = std::string("host: [ENGINE SYNTHESIZER] user settings: ") + up;
      longan::log_line(ul.c_str());
    }
  }
  const std::string tdir = game_dir + "\\.tiramisu";
  CreateDirectoryA(tdir.c_str(), nullptr);
  const int xlated = trt_pc2xb_translate_file(exe_path.c_str(),
                                              (tdir + "\\xb").c_str());
  if (xlated >= 0) {
    char xl_msg[192];
    snprintf(xl_msg, sizeof(xl_msg),
             "host: [PC->XBOX] translated %d import symbols -> %s\\xb_imports.json",
             xlated, tdir.c_str());
    longan::log_line(xl_msg);
  }
  // This is the click-to-play provisioning point: the profile above is
  // translated into registered graphics/audio/middleware bridges and a
  // fail-visible stub for every imported module the catalog does not know.
  // It runs before trt_runtime_init/guest mapping, so the first import bind
  // already sees the synthesized surface.
  char status_synth[256];
  snprintf(status_synth, sizeof(status_synth), "Provisioning: %s (%s / %s)",
           name.c_str(), trt_graphics_api_name(synth_prof.preferred_gfx),
           trt_audio_middleware_name(synth_prof.preferred_audio));
  set_guest_status(status_synth);
  trt_autonomous_provision_runtime(&be, &synth_prof);

  // Launch preflight: resolve the profile just provisioned against what this
  // host can actually do, so the wall is named once here instead of surfacing
  // 400 log lines later inside the engine. The verdict is written back into
  // .tiramisu/profile.json ("preflight") and summarised in one log line.
  {
    trt_host_caps caps{};
    caps.d3d11_device = trt_gpu_host_device_present();
    caps.d3d12_device = trt_gpu_host_d3d12_present();
    caps.compositor = trt_gpu_host_swapchain_present();
    caps.opengl_renderer = trt_gl_render_hooks_present();
    caps.mono_host = trt_mono_host_present();
    // The console host always has an audio endpoint (WASAPI/XAudio2 are
    // registered before this point); the offline harness passes 0.
    caps.audio_device = 1;
    trt_preflight_report pre{};
    const int blockers = trt_engine_launch_preflight(&synth_prof, &caps, &pre);
    char pl[640];
    if (blockers > 0) {
      snprintf(pl, sizeof(pl), "Preflight: %d blocker(s) - %s", blockers,
               pre.lines.empty() ? "see log" : pre.lines[0].c_str());
    } else {
      snprintf(pl, sizeof(pl), "Preflight: no known blocker (%s / %s)",
               trt_graphics_api_name(synth_prof.preferred_gfx),
               trt_audio_middleware_name(synth_prof.preferred_audio));
    }
    set_guest_status(pl);
  }

  const size_t exe_sl = exe_path.find_last_of("/\\");
  const std::string exe_dir =
      (exe_sl == std::string::npos) ? game_dir : exe_path.substr(0, exe_sl);
  // the runtime knows where the game's own DLLs live, and resolves imports
  // through the guest-image registry (a game's own DLLs beat the system's)
  trt_runtime_init(&be, exe_dir.c_str());
  if (!image_mounted && _stricmp(exe_dir.c_str(), game_dir.c_str()) != 0)
    trt_runtime_set_content_root(game_dir.c_str());
  trt_resolver* runtime_rs = trt_runtime_resolver();

  bool is_managed = synth_prof.is_managed;
  if (!is_managed) {
    // Fast header-only probe: read the first 64KB to check for CLR COM descriptor
    std::ifstream f(exe_path, std::ios::binary);
    if (f) {
      u8 hdr_buf[65536] = {};
      f.read(reinterpret_cast<char*>(hdr_buf), sizeof(hdr_buf));
      const size_t got = static_cast<size_t>(f.gcount());
      if (got >= 64 && trt_pe_is_managed(hdr_buf, got)) {
        is_managed = true;
      }
    }
  }

  if (is_managed) {
    longan::log_line("trt: managed exe detected, routing to Mono");
    trt_mono_config mcfg = {};
    mcfg.exe_path = exe_path.c_str();
    mcfg.log = [](const char* l) { longan::log_line(l); };
    // Console managed games use the Tiramisu FNA backend (host
    // CoreWindow + host input), never SDL.
    mcfg.fna_platform = "tiramisu";
    // FNA3D device handoff (Celeste hang fix): the Tiramisu backend
    // never calls FNA3D_PrepareWindowAttributes, so selectedDriver
    // stays -1 and FNA3D_CreateDevice returns NULL (silent no-render
    // spin). Attach the host D3D device + install FNA3D log hooks,
    // THEN select the driver. Both TOGETHER: PrepareWindowAttributes
    // without the attach takes the stock D3D11 path (garbage-HWND
    // swapchain -> AV).
    trt_trace("TRACE fna3d attach enter");
    {
      int arc = trt_fna3d_attach_from_hooks(
          [](const char* l) { longan::log_line(l); });
      char ab[128];
      snprintf(ab, sizeof(ab), "TRACE fna3d attach rc=%d attached=%d",
               arc, trt_fna3d_is_attached());
      trt_trace(ab);
      if (!trt_fna3d_is_attached()) {
        // FATAL guard: a failed attach must stop the launch here. Without
        // it, PrepareWindowAttributes takes the stock D3D11 probe path
        // (which succeeds on UWP) and CreateDevice builds a swapchain
        // against a garbage window handle -> NULL deref AV.
        longan::log_line("trt: FATAL fna3d attach failed - refusing stock D3D11 path");
        r.map_rc = TRT_ERR_FNA3D;
        strncpy(r.detail, trt_fna3d_last_error(), sizeof(r.detail) - 1);
        return r;
      }
    }
    trt_trace("TRACE PrepareWindowAttributes enter");
    FNA3D_PrepareWindowAttributes();
    trt_trace("TRACE PrepareWindowAttributes ok");
    checkpoint_phase(longan::launch_phase::managed_runtime);
    r.map_rc = trt_mono_run(&mcfg, nullptr, 0, nullptr);
    if (r.map_rc != TRT_OK) {
      strncpy(r.detail, trt_mono_last_error(), sizeof(r.detail) - 1);
    }
    return r;
  }

  trt_image* img = nullptr;
  {
    std::ifstream f(exe_path, std::ios::binary | std::ios::ate);
    if (!f) {
      r.map_rc = TRT_ERR_BAD_ARGS;
      snprintf(r.detail, sizeof(r.detail), "cannot open %s", exe_path.c_str());
      return r;
    }
    const std::streamoff size = f.tellg();
    if (size <= 0 || !f.seekg(0)) {
      r.map_rc = TRT_ERR_BAD_ARGS;
      snprintf(r.detail, sizeof(r.detail), "cannot size guest executable");
      return r;
    }

    std::vector<u8> buf;
    try {
      buf.resize(static_cast<size_t>(size));
    } catch (const std::bad_alloc&) {
      r.map_rc = TRT_ERR_ALLOC;
      snprintf(r.detail, sizeof(r.detail), "insufficient memory to load guest executable");
      return r;
    }
    if (!f.read(reinterpret_cast<char*>(buf.data()), size)) {
      r.map_rc = TRT_ERR_BAD_ARGS;
      snprintf(r.detail, sizeof(r.detail), "cannot read guest executable");
      return r;
    }

    snprintf(line, sizeof(line), "trt: launching %s (%zu bytes)", exe_path.c_str(),
             buf.size());
    longan::log_line(line);
    r.map_rc = trt_map_pe(&be, runtime_rs, buf.data(), buf.size(), &img);
  }
  log_guest_memory("after image load");
  if (r.map_rc != TRT_OK) {
    strncpy(r.detail, trt_errstr(r.map_rc), sizeof(r.detail) - 1);
    return r;
  }
  if (trt_seal_image(&be, img) != TRT_OK) {
    strncpy(r.detail, "seal failed", sizeof(r.detail) - 1);
    return r;
  }
  trt_images_register_main(img, effective_exe.c_str());
  if (effective_exe != exe) {
    trt_images_register_alias(img, exe.c_str());
  }
  if (img->is_pe32) {
    // 32-bit PE: run through x86 interpreter instead of native call
    snprintf(line, sizeof(line), "trt: 32-bit PE detected, running via x86 interpreter");
    longan::log_line(line);
    x86_context xctx = {};
    x86_cpu_init(&xctx);
    xctx.log = [](const char* s) { longan::log_line(s); };
    checkpoint_phase(longan::launch_phase::x86_execution);
    int rc = x86_run_pe32(&xctx, img->base, img->size_of_image,
                          img->guest_base, img->entry_rva, 0x00100000, 0x00100000);
    if (rc != X86_EXEC_OK) {
      r.map_rc = TRT_ERR_PE32_EXECUTION;
      snprintf(r.detail, sizeof(r.detail), "x86 stopped with status %d at EIP %08X",
               rc, xctx.regs.eip);
    } else {
      snprintf(r.detail, sizeof(r.detail), "x86 entry finished");
    }
    x86_mem_free(&xctx);
    return r;
  }
  run_guest_trapped(img, r.detail, sizeof(r.detail));
  return r;
}

// ---------------- Steam account sync & overlay bridge ----------------
void rescan_games();

void sync_steam_to_ui() {
  static ULONGLONG next_refresh = 0;
  const ULONGLONG now = GetTickCount64();
  if (now < next_refresh) return;
  next_refresh = now + 250;

  const auto acct = longan::steam_client_account();
  g_steam_status = acct.status_line;
  g_steam_qr = acct.qr_url;
  g_steam_busy = acct.busy;

  longan::ui_set_steam_account(
      acct.signed_in, acct.steamid64,
      u82ws(acct.persona.empty() ? acct.account_name : acct.persona),
      u82ws(acct.status_line));

  const auto raw_items = longan::steam_client_items();
  std::vector<longan::ui_steam_item> ui_items;
  ui_items.reserve(raw_items.size());
  for (const auto& r : raw_items) {
    longan::ui_steam_item u;
    u.title = u82ws(r.title);
    u.app_id = r.app_id;
    u.size_str = u82ws(r.size_str);
    u.installed = r.installed;
    u.downloading = r.downloading;
    u.queued = r.queued;
    u.download_pct = r.download_pct;
    u.status_str = u82ws(r.status_text);
    u.install_dir = u82ws(r.install_dir);
    ui_items.push_back(std::move(u));
  }
  longan::ui_set_steam_items(ui_items);

  std::vector<longan::ui_steam_friend> ui_friends;
  const int nf = trt_steam_get_friend_count();
  for (int i = 0; i < nf; ++i) {
    trt_steam_friend_info fi{};
    if (trt_steam_get_friend_at(i, &fi) == 0) {
      ui_friends.push_back({u82ws(fi.persona), fi.state, u82ws(fi.game_name)});
    }
  }
  longan::ui_set_steam_friends(ui_friends);

  std::vector<longan::ui_steam_ach> ui_achs;
  const int na = trt_steam_get_achievement_count();
  for (int i = 0; i < na; ++i) {
    trt_steam_achievement_info ai{};
    if (trt_steam_get_achievement_at(i, &ai) == 0) {
      ui_achs.push_back(
          {u82ws(ai.name), ai.achieved, ai.cur_progress, ai.max_progress});
    }
  }
  longan::ui_set_steam_achievements(ui_achs);
}

void steam_begin() {
  longan::steam_client_begin_qr([]() { g_steam_needs_rescan.store(true); });
}

// ---------------- dashboard: settings rows + screen nav ----------------
// Rows mirror the live host_settings; edited in place, applied+persisted on
// change. Value buffers are the ui_setting value[] arrays themselves (UTF-16,
// what the renderer draws).
const wchar_t* k_input_choices[4] = {L"auto", L"controller", L"kbm",
                                     L"pad2kbm"};
const wchar_t* k_cap_choices[4] = {L"0", L"30", L"60", L"120"};

void sync_settings_rows() {
  const auto s = longan::settings_get_global();
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
  wcscpy(g_set_rows[i].label, L"Shift+Tab quick menu");
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
  wcscpy(g_set_rows[i].label, L"Controller deadzone % (0-50)");
  g_set_rows[i].is_bool = false; g_set_rows[i].is_choice = false;
  g_set_rows[i].is_int = true;
  swprintf(g_set_rows[i].value, 64, L"%d", s.input_deadzone);
  ++i;
  // 16 audio buffer
  wcscpy(g_set_rows[i].label, L"Audio buffer ms (10-50)");
  g_set_rows[i].is_bool = false; g_set_rows[i].is_choice = false;
  g_set_rows[i].is_int = true;
  swprintf(g_set_rows[i].value, 64, L"%d", s.audio_buffer_ms);
  ++i;
  // 17 shader cache
  wcscpy(g_set_rows[i].label, L"Shader compile cache");
  set_bool(g_set_rows[i], s.shader_cache);
  ++i;
  // 18 resolution scale
  wcscpy(g_set_rows[i].label, L"Resolution scale % (50-200)");
  g_set_rows[i].is_bool = false; g_set_rows[i].is_choice = false;
  g_set_rows[i].is_int = true;
  swprintf(g_set_rows[i].value, 64, L"%d", s.resolution_scale);
  ++i;
  // 19 texture quality
  static const wchar_t* k_tex_choices[4] = {L"Low", L"Medium", L"High", L"Ultra"};
  wcscpy(g_set_rows[i].label, L"Texture quality");
  g_set_rows[i].is_bool = false; g_set_rows[i].is_choice = true;
  g_set_rows[i].is_int = false;
  wcscpy(g_set_rows[i].value, k_tex_choices[std::clamp(s.texture_quality, 0, 3)]);
  ++i;
  // 20 FMOD FSB5 decode
  wcscpy(g_set_rows[i].label, L"FMOD FSB5 audio decode");
  set_bool(g_set_rows[i], s.fsb_decode);
  ++i;
  g_set_count = i;
}

// Apply the current row buffers back into host_settings and persist.
void apply_settings_rows() {
  longan::host_settings s = longan::settings_get_global();
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
    else if (i == 18) s.resolution_scale = _wtoi(r.value);
    else if (i == 19) {
      if (wcscmp(r.value, L"Low") == 0) s.texture_quality = 0;
      else if (wcscmp(r.value, L"Medium") == 0) s.texture_quality = 1;
      else if (wcscmp(r.value, L"Ultra") == 0) s.texture_quality = 3;
      else s.texture_quality = 2;
    } else if (i == 20) s.fsb_decode = (wcscmp(r.value, L"1") == 0);
  }
  if (!longan::settings_store(s, g_local_state)) {
    sync_settings_rows();
    longan::ui_set_settings(g_set_rows, g_set_count, g_set_selected);
    longan::ui_toast(L"Could not save settings. Check free storage.", 4.0f);
    return;
  }
  apply_bridge_launch_settings();
  sync_settings_rows();
  longan::ui_set_settings(g_set_rows, g_set_count, g_set_selected);
  longan::ui_toast(L"Settings saved", 2.0f);
  longan::log_line("settings: saved from the TV dashboard");
}

void reset_all_settings() {
  longan::host_settings def{};
  if (!longan::settings_store(def, g_local_state)) {
    sync_settings_rows();
    longan::ui_set_settings(g_set_rows, g_set_count, g_set_selected);
    longan::ui_toast(L"Could not save settings. Check free storage.", 4.0f);
    return;
  }
  apply_bridge_launch_settings();
  sync_settings_rows();
  longan::ui_set_settings(g_set_rows, g_set_count, g_set_selected);
  longan::ui_toast(L"All settings reset to defaults", 2.8f);
  longan::log_line("settings: reset to defaults");
}

void request_settings_reset() {
  static ULONGLONG confirm_until = 0;
  const ULONGLONG now = GetTickCount64();
  if (confirm_until && now <= confirm_until) {
    confirm_until = 0;
    reset_all_settings();
    return;
  }
  confirm_until = now + 5000;
  longan::ui_toast(L"Select Reset Defaults again to confirm", 4.0f);
}


// Cycle/edit the selected row: dir = -1/+1 for LR cycles and int edit,
// toggle for bools. Returns true when something changed.
bool edit_setting_row(int dir) {
  if (g_set_selected < 0 || g_set_selected >= g_set_count) return false;
  longan::ui_setting& r = g_set_rows[g_set_selected];
  if (r.is_bool) {
    r.value[0] = (wcscmp(r.value, L"1") == 0) ? L'0' : L'1';
    r.value[1] = 0;
    return true;
  }
  if (r.is_choice) {
    static const wchar_t* k_tex_choices[4] = {L"Low", L"Medium", L"High", L"Ultra"};
    const wchar_t** list = (g_set_selected == 13) ? k_cap_choices
                         : (g_set_selected == 19) ? k_tex_choices
                                                  : k_input_choices;
    int cur = 0;
    for (int k = 0; k < 4; ++k)
      if (wcscmp(r.value, list[k]) == 0) cur = k;
    cur = (cur + dir + 4) % 4;
    wcscpy(r.value, list[cur]);
    return true;
  }
  if (r.is_int) {
    int step = 2, lo = 1, hi = 100;
    if (g_set_selected == 2) { step = 5; }
    else if (g_set_selected == 14) { step = 128; lo = 1024; hi = 8192; }
    else if (g_set_selected == 15) { step = 5; lo = 0; hi = 50; }
    else if (g_set_selected == 16) { step = 5; lo = 10; hi = 50; }
    else if (g_set_selected == 18) { step = 10; lo = 50; hi = 200; }
    int v = _wtoi(r.value) + dir * step;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    swprintf(r.value, 64, L"%d", v);
    return true;
  }
  return false;
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
  return 0x00020000u;  // runtime build 0.2.0
}

struct tiramisu_host
    : implements<tiramisu_host, IFrameworkViewSource, IFrameworkView> {
  CoreWindow m_window{nullptr};
  unsigned m_frames = 0;
  // Rescan debounce (item 4): upload-driven rescans arrive in bursts (one
  // per file); the pending rescan fires at most once per 2 s, scoped to
  // the changed game when every pending change is for the same game.
  bool m_rescan_pending = false;
  bool m_rescan_pending_full = false;
  std::string m_rescan_pending_game;
  ULONGLONG m_next_full_rescan_ms = 0;  // periodic root rescan (30 s)

  IFrameworkView CreateView() { return *this; }
  void Initialize(CoreApplicationView const&) {
    longan::log_line("host view: Initialize called");
  }
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
    // The FNA backend renders into this CoreWindow: hand its ABI
    // pointer to the native FNA layer (FNA's CreateWindow returns it;
    // the FNA3D D3D11 driver consumes it later).
    trt_fna_set_window_handle((void*)winrt::get_abi(window));
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
    // Opt into full-screen CoreWindow bounds. Keep the standard Arrow
    // available whenever the launcher is not drawing its own pointer.
    try {
      using namespace winrt::Windows::UI::ViewManagement;
      ApplicationViewScaling::TrySetDisableLayoutScaling(true);
      ApplicationView::GetForCurrentView().SetDesiredBoundsMode(
          ApplicationViewBoundsMode::UseCoreWindow);
    } catch (...) {
    }
    trt_input_display_size(static_cast<int>(sc.width),
                           static_cast<int>(sc.height));
    longan::ui_init(sc.width, sc.height, d2df, dwf, d2d_device_v,
                    longan::swapchain_dxgi());
    longan::swapchain_bind_d3d9();
    trt_input_set_cursor_pos(static_cast<int>(sc.width / 2),
                             static_cast<int>(sc.height / 2));

    m_kd = window.KeyDown([](CoreWindow const&, KeyEventArgs const& a) {
      g_controller_active.store(false);
      if (g_native_input.keyboard_active() && g_guest_running.load()) return;
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
      g_controller_active.store(false);
      g_last_mouse_move_ms.store(GetTickCount64());
      if (g_guest_running.load() && g_native_input.mouse_active_recent(300)) return;
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
      if (g_guest_running.load() && g_native_input.mouse_active_recent(300)) return;
      const auto point = a.CurrentPoint();
      feed_core_pointer(sender, point.Position());
      const int btn = host_mouse_btn_for_release(point.Properties().PointerUpdateKind());
      if (btn >= 0) trt_input_mouse_button(btn, false);
      if (btn == 0) launcher_click();
    });
    m_pm = window.PointerMoved([](CoreWindow const& sender,
                                  PointerEventArgs const& a) {
      g_controller_active.store(false);
      g_last_mouse_move_ms.store(GetTickCount64());
      if (g_guest_running.load() && g_native_input.mouse_active_recent(300)) return;
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
      g_controller_active.store(false);
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

  ULONGLONG m_last_web_pointer_ms = 0;

  void apply_web_controls() {
    const auto events = longan::webxfer_control_events();
    if (!events.empty()) g_controller_active.store(false);
    for (const auto& event : events) {
      if (event.kind == 'a' || event.kind == 'm' || event.kind == 'b')
        m_last_web_pointer_ms = GetTickCount64();
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
          static unsigned web_key_trace = 0;
          if (web_key_trace++ < 32) {
            char line[128];
            snprintf(line, sizeof(line), "input: web key vk=%u down=%u scan=%u",
                     (unsigned)vk, down ? 1u : 0u, (unsigned)(scan & 0xff));
            longan::log_line(line);
          }
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

  void trigger_steam_item_action(unsigned app_id) {
    if (!app_id) return;
    const auto items = longan::steam_client_items();
    const auto found = std::find_if(items.begin(), items.end(),
        [app_id](const auto& item) { return item.app_id == app_id; });
    if (found == items.end()) return;
    const auto& it = *found;
    if (it.downloading || it.queued) {
      if (longan::steam_client_cancel_download(app_id))
        longan::ui_toast(L"Steam download cancellation requested", 2.5f);
      return;
    }
    if (it.installed) {
      std::string launch_name;
      for (const auto& ge : g_games) {
        if (!it.install_dir.empty() &&
            _stricmp(ge.dir.c_str(), it.install_dir.c_str()) == 0) {
          launch_name = ge.name;
          break;
        }
      }
      if (launch_name.empty() && !it.install_dir.empty()) {
        rescan_games();
        for (const auto& ge : g_games) {
          if (_stricmp(ge.dir.c_str(), it.install_dir.c_str()) == 0) {
            launch_name = ge.name;
            break;
          }
        }
      }
      if (launch_name.empty()) launch_name = it.title;
      g_pending_launch = launch_name;
    } else if (!it.downloading) {
      if (!longan::steam_client_account().signed_in) {
        longan::ui_toast(L"Sign in with Steam to install this game", 3.0f);
        return;
      }
      if (!longan::steam_client_download_app(
              it.app_id, []() { g_steam_needs_rescan.store(true); }))
        longan::ui_toast(L"Could not queue Steam install", 3.0f);
    }
  }

  void sync_tools_list() {
    std::vector<longan::ui_tool_item> items = {
        {L"Rescan Game Library", L"Internal and accessible USB game folders", L"RUN"},
        {L"Refresh Game Cover Art", L"Refresh available cover art", L"RUN"},
        {L"Run Runtime Self-Test", L"Verify PE64 loader & syscall hooks", L"RUN"},
        {L"Gamepad-as-Mouse Mode", L"Right stick moves cursor (R3 / F1)",
         g_mouse_mode ? L"ON" : L"OFF"},
        {L"Keyboard Cursor Mode", L"Arrows/Numpad move cursor (F8)",
         g_kbd_mouse_mode ? L"ON" : L"OFF"},
        {L"Open On-Screen Keyboard", L"Show Xbox virtual keyboard", L"OPEN"},
        {L"Refresh Storage & Health", L"Update disk space & system diagnostics", L"RUN"},
    };
    longan::ui_set_tools(items, longan::ui_tools_selected(), g_tool_result);
  }

  void run_console_tool(int idx) {
    switch (idx) {
      case 0:
        poll_external_storage(true);
        rescan_games();
        g_tool_result = L"Rescanned: " + std::to_wstring(g_games.size()) + L" games";
        break;
      case 1:
        longan::cover_art_reset_queue();
        rescan_games();
        g_tool_result = L"Cover art refresh queued for " +
                        std::to_wstring(g_games.size()) + L" games";
        break;
      case 2: {
        const auto gr = run_game("selftest", "guest_hello.exe", package_install_path());
        g_tool_result = L"Self-test: " + u82ws(trt_errstr(gr.map_rc));
        break;
      }
      case 3:
        g_mouse_mode = !g_mouse_mode;
        g_tool_result = g_mouse_mode ? L"Gamepad-as-Mouse ON" : L"Gamepad-as-Mouse OFF";
        break;
      case 4:
        g_kbd_mouse_mode = !g_kbd_mouse_mode;
        refresh_control_hint();
        g_tool_result = g_kbd_mouse_mode ? L"KBM Cursor ON (F8)" : L"KBM Cursor OFF";
        break;
      case 5:
        try {
          winrt::Windows::UI::ViewManagement::InputPane::GetForCurrentView().TryShow();
          g_tool_result = L"On-Screen Keyboard requested";
        } catch (...) {
          g_tool_result = L"On-Screen Keyboard unavailable";
        }
        break;
      case 6:
        refresh_storage_stats();
        run_probes();
        g_tool_result = L"Storage & hardware probes refreshed";
        break;
      default:
        break;
    }
    sync_tools_list();
    longan::ui_sfx_confirm();
    if (!g_tool_result.empty()) longan::ui_toast(g_tool_result, 2.8f);
  }

  void handle_delete_game(int idx) {
    if (idx < 0 || idx >= static_cast<int>(g_games.size())) return;
    const ULONGLONG now = GetTickCount64();
    if (g_del_confirm_idx == idx && (now - g_del_confirm_ms) < 5500ull) {
      const std::string name = g_games[idx].name;
      const std::string dir = g_games[idx].dir;
      const std::string img = g_games[idx].image;
      g_del_confirm_idx = -1;
      g_del_confirm_ms = 0;
      longan::ui_set_delete_confirm(-1);
      bool ok = false;
      if (!img.empty()) {
        ok = DeleteFileA(img.c_str()) != 0;
      } else if (!dir.empty() && dir.size() > g_games_dir.size()) {
        ok = delete_dir_recursive_host(dir);
      }
      longan::ui_sfx_delete();
      rescan_games();
      refresh_storage_stats();
      longan::ui_toast(
          ok ? (L"Uninstalled " + u82ws(name))
             : (L"Could not uninstall " + u82ws(name)),
          3.2f);
    } else {
      g_del_confirm_idx = idx;
      g_del_confirm_ms = now;
      longan::ui_set_delete_confirm(idx);
      longan::ui_sfx_confirm();
      longan::ui_toast(
          L"Click Uninstall again within 5s to delete " + u82ws(g_games[idx].name),
          3.2f);
    }
  }

  void execute_quick_menu_action(int qsel) {
    if (qsel == 0) {
      g_quick_menu_open = false;
      longan::ui_set_quick_menu(false);
    } else if (qsel == 1 || qsel == 4) {
      longan::steam_client_refresh_remote(
          []() { g_steam_needs_rescan.store(true); });
      longan::ui_toast(L"Refreshing Steam profile & friends…", 2.5f);
    } else if (qsel == 3) {
      g_mouse_mode = !g_mouse_mode;
      longan::ui_toast(g_mouse_mode ? L"Pad-as-Mouse ON" : L"Pad-as-Mouse OFF", 2.2f);
    } else if (qsel == 5) {
      g_quick_menu_open = false;
      longan::ui_set_quick_menu(false);
      request_guest_stop();
    }
  }

  void launcher_click() {
    if (longan::boot_anim_is_active()) {
      longan::boot_anim_skip();
      return;
    }
    if (g_guest_running.load() && !g_quick_menu_open) return;
    g_controller_active.store(false);
    int x, y;
    trt_mouse_pos(&x, &y);
    const auto hit = longan::ui_hit_test(x, y);
    switch (hit.type) {
      case longan::ui_click_type::quick_menu_item:
        execute_quick_menu_action(hit.index);
        break;
      case longan::ui_click_type::open_home:
        if (g_setup_active && g_setup_done) {
          g_setup_active = false;
          longan::ui_dismiss_first_run();
        }
        g_screen = longan::UI_SHELF;
        longan::ui_set_screen(g_screen);
        break;
      case longan::ui_click_type::tab:
        if (g_setup_active) break;
        g_screen = static_cast<longan::ui_screen>(hit.index);
        longan::ui_set_screen(g_screen);
        if (g_screen == longan::UI_SETTINGS) {
          sync_settings_rows();
          longan::ui_set_settings(g_set_rows, g_set_count, g_set_selected);
        } else if (g_screen == longan::UI_TOOLS) {
          sync_tools_list();
        } else if (g_screen == longan::UI_STEAM) {
          const auto acct = longan::steam_client_account();
          if (!acct.signed_in && !acct.busy && acct.qr_url.empty()) {
            steam_begin();
          }
        }
        break;
      case longan::ui_click_type::open_transfer:
        g_setup_active = false;
        longan::ui_dismiss_first_run();
        g_screen = longan::UI_TOOLS;
        longan::ui_set_screen(g_screen);
        sync_tools_list();
        break;
      case longan::ui_click_type::select_game:
        if (hit.index >= 0 && hit.index < static_cast<int>(g_games.size()))
          g_selected = hit.index;
        break;
      case longan::ui_click_type::play:
        if (hit.index >= 0 && hit.index < static_cast<int>(g_games.size())) {
          g_pending_launch = g_games[hit.index].name;
          longan::ui_sfx_launch();
          longan::ui_toast(L"Launching " + u82ws(g_games[hit.index].name) + L"…", 3.0f);
        }
        break;
      case longan::ui_click_type::cycle_exe:
        longan::ui_sfx_confirm();
        cycle_selected_exe(hit.index, 1);
        break;
      case longan::ui_click_type::cycle_input:
        longan::ui_sfx_confirm();
        cycle_selected_input(hit.index);
        break;
      case longan::ui_click_type::delete_game:
        handle_delete_game(hit.index);
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
      case longan::ui_click_type::select_settings_section:
        if (hit.index >= 0 && hit.index < g_set_count) {
          g_set_selected = hit.index;
          longan::ui_set_settings(g_set_rows, g_set_count, g_set_selected);
        }
        break;
      case longan::ui_click_type::reset_settings:
        request_settings_reset();
        break;
      case longan::ui_click_type::select_steam:
        break;
      case longan::ui_click_type::steam_action:
        trigger_steam_item_action(longan::ui_steam_selected_app());
        break;
      case longan::ui_click_type::steam_qr:
        steam_begin();
        break;
      case longan::ui_click_type::run_tool:
        run_console_tool(hit.index);
        break;
      case longan::ui_click_type::none:
        break;
    }
  }

  void boot_all() {
    const std::string install = package_install_path();
    g_package_games_dir = install.empty()
        ? std::string()
        : install + std::string(1, static_cast<char>(92)) + "network-games";
    g_local_state =
        ws2u8(winrt::Windows::Storage::ApplicationData::Current()
                  .LocalFolder()
                  .Path());
    // Managed (FNA) games save under LocalState.
    trt_fna_set_storage_root(g_local_state.c_str());
    g_games_dir = g_local_state + "\\games";
    CreateDirectoryA(g_games_dir.c_str(), nullptr);
    CreateDirectoryA((g_local_state + "\\incoming").c_str(), nullptr);
    longan::cover_art_init(g_local_state);
    refresh_storage_stats();
    longan::log_open(install);
    char line[256];
    snprintf(line, sizeof(line), "tiramisu boot: pkg=%s state=%s",
             install.c_str(), g_local_state.c_str());
    longan::log_line(line);

    // 1ms host timer resolution for the session: guest Sleep(1) and frame
    // pacing need it (the Windows default is ~15.6ms).
    ::timeBeginPeriod(1);
    longan::log_line("tiramisu boot: host timer resolution 1ms (timeBeginPeriod)");

    // ---- persisted settings (dashboard-editable; consumed by bridges) ----
    // Load first: apply_console_hardware_profile starts from the stored
    // settings and must not overwrite them with defaults on every boot.
    const int applied = longan::settings_load(g_local_state);
    g_network_games_dir = longan::settings_get().network_games_root;
    longan::webxfer_set_network_games_dir(g_network_games_dir);
    if (!g_network_games_dir.empty()) {
      const std::string root_line =
          "games: external network root=" + g_network_games_dir;
      longan::log_line(root_line.c_str());
    }
    // Auto-apply detected console hardware profile (memory budget, ESRAM
    // budget ledger, and resolution/texture scaling) on top of those settings.
    const auto cons_info = longan::get_detected_console_info();
    longan::apply_console_hardware_profile(cons_info, g_local_state);
    apply_bridge_launch_settings();
    snprintf(line, sizeof(line), "settings: %d keys applied from settings.ini",
             applied);
    longan::log_line(line);

    // System specs for overlay (read once at boot)
    {
      wchar_t esram_tag[64] = L"";
      if (cons_info.has_esram) {
        swprintf(esram_tag, 64, L" + %dMB ESRAM", cons_info.esram_size_mb);
      }
      wchar_t info[256];
      swprintf(info, 256, L"%hs (%hs) | %hs @ %.2f GHz | %.0f GB RAM%ls",
               cons_info.generation_name, cons_info.codename,
               cons_info.cpu_arch, cons_info.cpu_clock_ghz,
               (double)cons_info.total_ram_mb / 1024.0, esram_tag);
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
    longan::webxfer_set_package_games_dir(g_package_games_dir);
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
    // -1 means the query itself is unavailable; keep the optimistic default.
    if (mouse >= 0) g_host_mouse_present.store(mouse != 0);
    if (mouse == 0) {
      longan::log_line(
          "input: no hardware pointer presented to this package -> pad-driven "
          "pointer will be used for mouse-driven titles");
    }
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
    poll_external_storage(true);
    rescan_games();
    longan::steam_client_init(g_local_state, g_games_dir);
    trt_steam_load_account(g_local_state.c_str());
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
    g_setup_done = true;
    sync_settings_rows();
    longan::ui_set_settings(g_set_rows, g_set_count, g_set_selected);
    sync_tools_list();
    // Land straight on the Home dashboard beneath the HD startup boot animation
    g_setup_active = false;
    longan::ui_dismiss_first_run();
    g_screen = longan::UI_SHELF;
    longan::ui_set_screen(g_screen);
    if (!install.empty()) {
      longan::boot_anim_start(install + "\\boot_anim.tanim");
    }
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
          g_controller_active.store(false);
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
          g_controller_active.store(false);
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
          const auto d = a.MouseDelta();
          if (d.X == 0 && d.Y == 0) return;
          const ULONGLONG now = GetTickCount64();
          g_controller_active.store(false);
          g_last_mouse_move_ms.store(now);
          g_last_raw_mouse_ms.store(now);
          if (!g_uwp_raw_mouse.exchange(true))
            longan::log_line("input: physical relative mouse motion received");
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
    CoreCursor system_arrow{nullptr};
    try {
      system_arrow = CoreCursor(CoreCursorType::Arrow, 0);
      active_window.PointerCursor(system_arrow);
    } catch (...) {}
    bool software_hardware_pointer = false;
    bool cursor_update_failed = false;
    if (!g_booted) {
      g_booted = true;
      boot_all();
      g_native_input.set_host_key_handler(&handle_host_cursor_key);
      g_native_input.open();
    }
    setup_mouse_device();
    longan::log_line(active_window.IsInputEnabled()
                         ? "input: CoreWindow initially enabled"
                         : "input: CoreWindow initially disabled");
    longan::log_line(m_mouse_device_available
                         ? "input: MouseDevice relative listener registered"
                         : "input: MouseDevice unavailable; CoreWindow pointer listener active");
    enable_keyboard_interceptor();

    auto next_heartbeat = std::chrono::steady_clock::now();
    g_boot_ms = (ULONGLONG)GetTickCount64();
    while (!g_shutdown.load()) {
      // Heartbeat (item 10): a line every 60 s so a wedged app is
      // distinguishable from a quiet one in tiramisu.log. Wall clock, not
      // frame count — m_frames freezes while a guest runs (Sleep(4) +
      // continue skips the increment), which is exactly when the
      // heartbeat matters most.
      {
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_heartbeat) {
          next_heartbeat = now + std::chrono::seconds(60);
          char hb[256];
          snprintf(hb, sizeof(hb), "heartbeat: up %llus guest=%s shelf=%zu games",
                   (unsigned long long)((GetTickCount64() - g_boot_ms) / 1000),
                   g_running_game_name.empty() ? "(none)"
                                               : g_running_game_name.c_str(),
                   g_games.size());
          longan::log_line(hb);
        }
      }
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
                    a_btn = false, b_btn = false, x_btn = false, y_btn = false,
                    rs_btn = false, vkbd_btn = false;
        static bool lb_btn = false, rb_btn = false;
        static int prev_mx = -1, prev_my = -1;
        static bool mm_was_active = false, mm_was_rt = false, mm_was_lt = false,
                    mm_was_rb = false, mm_was_lb = false, mm_was_ls = false,
                    mm_was_a = false, mm_was_b = false, mm_was_x = false,
                    mm_was_y = false, mm_was_dl = false, mm_was_dr = false,
                    mm_was_w = false, mm_was_s = false, mm_was_ak = false,
                    mm_was_d = false, guest_was_running = false;
        static ULONGLONG mm_next_wheel_ms = 0;
        static int mm_wheel_direction = 0;
        static ULONGLONG mm_last_motion_ms = 0;
        static float mm_px = 0.f, mm_py = 0.f;
        static bool back_start_was = false;
        const u16 b = pad_ok ? xs.Gamepad.wButtons : 0;

        // Adaptive Controller vs KBM detection + CoreWindow::PointerPosition + Win32 GetCursorPos fallback:
        const ULONGLONG now_input_ms = GetTickCount64();
        {
          // 1) WinRT CoreWindow::PointerPosition() poll
          if (m_window) {
            try {
              static float last_cw_x = -9999.0f, last_cw_y = -9999.0f;
              const auto pos = m_window.PointerPosition();
              const auto bnd = m_window.Bounds();
              if (bnd.Width > 0 && bnd.Height > 0) {
                const float rx = pos.X - bnd.X;
                const float ry = pos.Y - bnd.Y;
                if (last_cw_x > -9000.0f &&
                    (fabsf(rx - last_cw_x) > 0.5f || fabsf(ry - last_cw_y) > 0.5f)) {
                  int dw = 1920, dh = 1080;
                  trt_display_size(&dw, &dh);
                  const int cx = std::clamp((int)(rx * dw / bnd.Width), 0, dw - 1);
                  const int cy = std::clamp((int)(ry * dh / bnd.Height), 0, dh - 1);
                  trt_input_pointer_move(cx, cy, false);
                  g_controller_active.store(false);
                  g_last_mouse_move_ms.store(now_input_ms);
                }
                last_cw_x = rx;
                last_cw_y = ry;
              }
            } catch (...) {
            }
          }
          // 2) Direct user32 GetCursorPos + GetAsyncKeyState probe (catches the
          //    desktop/Xbox pointer even when WinRT PointerMoved never fires).
          //    This runs in-game too: a title that polls the cursor must not be
          //    left with a frozen pointer just because CoreWindow is silent. It
          //    yields whenever CoreWindow pointer events are live.
          const ULONGLONG last_core = g_last_core_pointer_ms.load();
          const bool core_pointer_recent =
              last_core != 0 && (now_input_ms - last_core) < 500;
          {
            using get_cp_fn = BOOL(WINAPI*)(POINT*);
            using get_aks_fn = SHORT(WINAPI*)(int);
            static get_cp_fn p_get_cp = nullptr;
            static get_aks_fn p_get_aks = nullptr;
            static bool u32_probed = false;
            static POINT last_pt{-99999, -99999};
            static bool was_btn[3] = {false, false, false};
            if (!u32_probed) {
              u32_probed = true;
              HMODULE u32 = GetModuleHandleW(L"user32.dll");
              if (!u32) u32 = LoadLibraryW(L"user32.dll");
              if (u32) {
                p_get_cp = reinterpret_cast<get_cp_fn>(GetProcAddress(u32, "GetCursorPos"));
                p_get_aks = reinterpret_cast<get_aks_fn>(GetProcAddress(u32, "GetAsyncKeyState"));
              }
            }
            if (p_get_cp && !core_pointer_recent) {
              POINT pt{};
              if (p_get_cp(&pt)) {
                if (last_pt.x != -99999 && (pt.x != last_pt.x || pt.y != last_pt.y)) {
                  int dw = 1920, dh = 1080;
                  trt_display_size(&dw, &dh);
                  const int cx = std::clamp((int)pt.x, 0, dw - 1);
                  const int cy = std::clamp((int)pt.y, 0, dh - 1);
                  trt_input_pointer_move(cx, cy, false);
                  g_controller_active.store(false);
                  g_last_mouse_move_ms.store(now_input_ms);
                }
                last_pt = pt;
              }
            }
            if (p_get_aks && !core_pointer_recent) {
              static const u16 k_probe_vk[3] = {0x01 /*VK_LBUTTON*/,
                                                0x02 /*VK_RBUTTON*/,
                                                0x04 /*VK_MBUTTON*/};
              for (int i = 0; i < 3; ++i) {
                const bool down = (p_get_aks(k_probe_vk[i]) & 0x8000) != 0;
                if (down == was_btn[i]) continue;
                was_btn[i] = down;
                g_controller_active.store(false);
                if (trt_mouse_button(i) != down) trt_input_mouse_button(i, down);
                if (i == 0 && !down && !g_guest_running.load()) launcher_click();
              }
            }
          }
        }
        if (!g_mouse_mode) {
          int cur_mx = 0, cur_my = 0;
          trt_mouse_pos(&cur_mx, &cur_my);
          if (prev_mx >= 0 && (cur_mx != prev_mx || cur_my != prev_my)) {
            g_controller_active.store(false);
            g_last_mouse_move_ms.store(now_input_ms);
          }
          prev_mx = cur_mx;
          prev_my = cur_my;
        }
        // Hot-plug: the shell can attach the pointer after launch. Trust either
        // GameInput's device status or a fresh CoreWindow pointer event, then
        // hand the cursor from the pad to the real mouse in auto mode.
        if (!g_host_mouse_present.load() &&
            (g_native_input.mouse_connected() || g_last_core_pointer_ms.load() != 0 ||
             g_last_raw_mouse_ms.load() != 0)) {
          g_host_mouse_present.store(true);
          longan::log_line("input: mouse connected -> hardware pointer active");
        }
        if (g_host_mouse_present.load() && g_auto_pad_mouse.load() &&
            g_guest_running.load()) {
          g_auto_pad_mouse.store(false);
          g_mouse_mode = false;
          longan::log_line("input: hardware mouse active -> pad-as-mouse OFF");
        }
        if (g_native_input.mouse_active() || g_native_input.keyboard_active() ||
            (!pad_ok && (g_native_input.mouse_connected() ||
                         g_native_input.keyboard_connected()))) {
          g_controller_active.store(false);
        }
        // Any keyboard launcher key switches to KBM mode
        if (launcher_key(0x09) || launcher_key(0x0D) || launcher_key(0x1B) ||
            launcher_key(0x20) || launcher_key(0x25) || launcher_key(0x26) ||
            launcher_key(0x27) || launcher_key(0x28) || launcher_key('W') ||
            launcher_key('A') || launcher_key('S') || launcher_key('D')) {
          g_controller_active.store(false);
        }
        // Real gamepad input switches back to Controller mode (high stick
        // threshold of 20000 prevents stick drift from hiding the mouse cursor)
        if (pad_ok) {
          const bool pad_input =
              (b != 0) ||
              std::abs((int)xs.Gamepad.sThumbLX) > 20000 ||
              std::abs((int)xs.Gamepad.sThumbLY) > 20000 ||
              std::abs((int)xs.Gamepad.sThumbRX) > 20000 ||
              std::abs((int)xs.Gamepad.sThumbRY) > 20000 ||
              xs.Gamepad.bLeftTrigger > 80 ||
              xs.Gamepad.bRightTrigger > 80;
          if (pad_input) g_controller_active.store(true);
        } else {
          // If no controller is connected at all, default to KBM + visible cursor!
          g_controller_active.store(false);
        }
        longan::ui_set_input_mode(g_controller_active.load(), pad_ok);
        {
          int pmx = -1000, pmy = -1000;
          trt_mouse_pos(&pmx, &pmy);
          longan::ui_set_pointer_pos(pmx, pmy);
        }
        const bool guest_running = g_guest_running.load();
        const bool hw_pointer = g_host_mouse_present.load() ||
                                g_native_input.mouse_connected();
        // Use our high-contrast arrow only while absolute hardware events
        // are arriving. If the broker stops relaying them, restore the system
        // arrow instead of leaving a frozen or invisible cursor.
        const ULONGLONG absolute_event = g_last_core_pointer_ms.load();
        const bool can_draw_hardware = system_arrow && !guest_running && g_input_focused &&
            hw_pointer && !g_controller_active.load() && absolute_event &&
            now_input_ms - absolute_event < 2500 && !longan::boot_anim_is_active() &&
            longan::ui_can_draw_cursor();
        if (!cursor_update_failed && can_draw_hardware != software_hardware_pointer) {
          try {
            active_window.PointerCursor(can_draw_hardware ? CoreCursor{nullptr} : system_arrow);
            software_hardware_pointer = can_draw_hardware;
          } catch (...) {
            cursor_update_failed = true;
            try { active_window.PointerCursor(system_arrow); } catch (...) {}
            software_hardware_pointer = false;
          }
        }
        const bool virtual_pointer = !hw_pointer &&
            (g_mouse_mode || g_kbd_mouse_mode ||
             (m_last_web_pointer_ms && now_input_ms - m_last_web_pointer_ms < 12000));
        longan::ui_set_cursor_visible(software_hardware_pointer ||
                                     (!guest_running && virtual_pointer));
        longan::set_launcher_cursor_visible(false);

        // Launcher mouse wheel scrolling across all screens
        if (!g_guest_running.load() && !g_setup_active) {
          const i32 wh = trt_mouse_wheel();
          if (wh != 0) {
            const int dir = (wh > 0) ? -1 : 1;
            if (g_screen == longan::UI_SETTINGS && g_set_count > 0) {
              g_set_selected = longan::ui_settings_step(dir);
              longan::ui_set_settings(g_set_rows, g_set_count, g_set_selected);
            } else if (g_screen == longan::UI_STEAM) {
              if (dir < 0) longan::ui_steam_nav_up();
              else longan::ui_steam_nav_down();
            } else if (g_screen == longan::UI_TOOLS) {
              longan::ui_log_scroll(dir < 0 ? 4 : -4);
            } else if ((g_screen == longan::UI_SHELF || g_screen == longan::UI_LIBRARY) &&
                       !g_games.empty()) {
              g_selected = std::clamp(g_selected + dir, 0, (int)g_games.size() - 1);
            }
          }
        }

        // RB/LB cycle dashboard screens (Home -> Library -> Steam -> Tools -> Settings)
        // on the launcher; keyboard: Tab / Shift+Tab.
        auto on_screen_enter = [&](longan::ui_screen s) {
          g_screen = s;
          longan::ui_set_screen(g_screen);
          if (g_screen == longan::UI_SETTINGS) {
            sync_settings_rows();
            longan::ui_set_settings(g_set_rows, g_set_count, g_set_selected);
          } else if (g_screen == longan::UI_TOOLS) {
            sync_tools_list();
          } else if (g_screen == longan::UI_STEAM) {
            const auto acct = longan::steam_client_account();
            if (!acct.signed_in && !acct.busy && acct.qr_url.empty()) {
              steam_begin();
            }
          }
        };
        const bool want_rb = (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) ||
                             (launcher_key(0x09 /*VK_TAB*/) &&
                              !trt_key_down(0x10 /*VK_SHIFT*/));
        const bool want_lb = (b & XINPUT_GAMEPAD_LEFT_SHOULDER) ||
                             (trt_key_down(0x10 /*VK_SHIFT*/) &&
                              launcher_key(0x09 /*VK_TAB*/));
        if (!g_guest_running.load() && g_setup_active == false) {
          if (want_rb && !rb_btn) {
            rb_btn = true;
            on_screen_enter((longan::ui_screen)(((int)g_screen + 1) % longan::UI_TAB_COUNT));
          } else if (!want_rb) rb_btn = false;
          if (want_lb && !lb_btn) {
            lb_btn = true;
            on_screen_enter((longan::ui_screen)(((int)g_screen + longan::UI_TAB_COUNT - 1) %
                                                longan::UI_TAB_COUNT));
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
        const bool ls_up = pad_ok && !g_mouse_mode && xs.Gamepad.sThumbLY > 15000;
        const bool ls_dn = pad_ok && !g_mouse_mode && xs.Gamepad.sThumbLY < -15000;
        const bool ls_lt = pad_ok && !g_mouse_mode && xs.Gamepad.sThumbLX < -15000;
        const bool ls_rt = pad_ok && !g_mouse_mode && xs.Gamepad.sThumbLX > 15000;
        const bool want_up = (b & XINPUT_GAMEPAD_DPAD_UP) || ls_up ||
                             (!km_lock && (launcher_key(0x26 /*VK_UP*/) || launcher_key('W')));
        const bool want_dn = (b & XINPUT_GAMEPAD_DPAD_DOWN) || ls_dn ||
                             (!km_lock && (launcher_key(0x28 /*VK_DOWN*/) || launcher_key('S')));
        const bool want_lt = (b & XINPUT_GAMEPAD_DPAD_LEFT) || ls_lt ||
                             (!km_lock && (launcher_key(0x25 /*VK_LEFT*/) || launcher_key('A')));
        const bool want_rt = (b & XINPUT_GAMEPAD_DPAD_RIGHT) || ls_rt ||
                             (!km_lock && (launcher_key(0x27 /*VK_RIGHT*/) || launcher_key('D')));
        const bool want_go = (b & XINPUT_GAMEPAD_A) ||
                             (!km_lock && (launcher_key(0x0D /*VK_RETURN*/) ||
                                           launcher_key(0x20 /*VK_SPACE*/)));
        const bool want_back = (b & XINPUT_GAMEPAD_B) ||
                               (!km_lock && launcher_key(0x1B /*VK_ESCAPE*/));
        const bool want_x = ((b & XINPUT_GAMEPAD_X) && !(b & XINPUT_GAMEPAD_BACK)) ||
                            (!km_lock && launcher_key('X'));
        const bool want_st = (b & XINPUT_GAMEPAD_Y) || (!km_lock && launcher_key('Y'));
        const bool pad_vm = (b & XINPUT_GAMEPAD_BACK) && (b & XINPUT_GAMEPAD_START);
        if (g_guest_running.load()) {
          if (longan::webxfer_poll_stop()) request_guest_stop();
          // Force-kill watchdog (item 5): a stop request a hung guest
          // ignores for >10 s escalates to TerminateThread. The stop trap
          // clears the flag when the guest unwinds, and run_game clears it
          // per launch, so a stale flag can never trigger this.
          if (g_guest_stop_req.load() &&
              (u64)GetTickCount64() - g_guest_stop_since.load() > 10000ull) {
            force_kill_guest("stop ignored 10s");
          }
          // Back+Start exits the running game immediately. It used to toggle
          // the quick menu, which made the requested shortcut indistinguishable
          // from opening an overlay. The quick menu remains available on
          // Shift+Tab and through Steam's overlay shortcut.
          if (pad_vm && !back_start_was) {
            back_start_was = true;
            if (g_quick_menu_open) {
              g_quick_menu_open = false;
              longan::ui_set_quick_menu(false);
            }
            request_guest_stop();
          } else if (!pad_vm) {
            back_start_was = false;
          }
          static bool qm_was = false;
          const bool shift_tab =
              trt_key_down(0x10 /*VK_SHIFT*/) && trt_key_down(0x09 /*VK_TAB*/);
          if (longan::trt_settings_quick_menu_enabled() &&
              (shift_tab || trt_steam_overlay_check_and_reset())) {
            // Shift+Tab or the Steam API toggles the quick menu. Back+Start is
            // reserved for exiting the game (above).
            if (!qm_was) {
              qm_was = true;
              g_quick_menu_open = !g_quick_menu_open;
              longan::ui_set_quick_menu(g_quick_menu_open);
            }
          } else {
            qm_was = false;
          }
        } else {
          back_start_was = false;
          longan::webxfer_poll_stop();
          if (guest_was_running) {
            trt_input_release_all();
            trt_input_reset_cursor_mode(false);
            g_mouse_mode = false;
            longan::webxfer_set_running_game("");
            const std::string gs = get_guest_status();
            if (!gs.empty() && gs != "idle") longan::ui_toast(u82ws(gs), 3.6f);
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
        // Console-standard directional auto-repeat (340ms initial, 95ms repeat)
        static ULONGLONG rep_up = 0, rep_dn = 0, rep_lt = 0, rep_rt = 0;
        auto nav_pulse = [&](bool held, bool& latched, ULONGLONG& next_ms) -> bool {
          if (!held) { latched = false; next_ms = 0; return false; }
          if (!latched) { latched = true; next_ms = now_input_ms + 340ull; return true; }
          if (now_input_ms >= next_ms) { next_ms = now_input_ms + 95ull; return true; }
          return false;
        };
        const bool step_up = nav_pulse(want_up, d_up, rep_up);
        const bool step_dn = nav_pulse(want_dn, d_dn, rep_dn);
        const bool step_lt = nav_pulse(want_lt, d_lt, rep_lt);
        const bool step_rt = nav_pulse(want_rt, d_rt, rep_rt);

        if (longan::boot_anim_is_active()) {
          // Allow skipping the startup boot animation cleanly with any face button / key
          const bool skip_btn =
              (b & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_START)) != 0 ||
              launcher_key(0x0D /*VK_RETURN*/) || launcher_key(0x20 /*VK_SPACE*/) ||
              launcher_key(0x1B /*VK_ESCAPE*/);
          if (skip_btn) {
            longan::boot_anim_skip();
            a_btn = b_btn = true;  // latch so skip press never launches a game
          }
        } else if (!g_guest_running.load() && g_screen == longan::UI_SHELF) {
          // Home screen: Left/Right (or Up/Down) moves across Continue Playing cards,
          // A launches (or opens Tools if empty), X cycles exe, Y jumps to Tools/Transfer.
          if ((step_lt || step_up) && !g_games.empty() && g_selected > 0) --g_selected;
          if ((step_rt || step_dn) && !g_games.empty() && g_selected + 1 < (int)g_games.size()) ++g_selected;
          if (want_go && !a_btn) {
            a_btn = true;
            if (g_setup_active && g_setup_done) {
              g_setup_active = false;
              longan::ui_dismiss_first_run();
            } else if (!g_setup_active && !g_games.empty()) {
              g_pending_launch = g_games[g_selected].name;
              longan::ui_sfx_launch();
              longan::ui_toast(L"Launching " + u82ws(g_games[g_selected].name) + L"…", 3.0f);
            } else if (!g_setup_active && g_games.empty()) {
              on_screen_enter(longan::UI_TOOLS);
            }
          } else if (!want_go) a_btn = false;
          if (want_x && !x_btn) {
            x_btn = true;
            longan::ui_sfx_confirm();
            cycle_selected_exe(g_selected, 1);
          } else if (!want_x) x_btn = false;
          if (want_st && !y_btn) {
            y_btn = true;
            on_screen_enter(longan::UI_TOOLS);
          } else if (!want_st) y_btn = false;
          b_btn = want_back;
        } else if (!g_guest_running.load() && g_screen == longan::UI_LIBRARY) {
          // Clear expired 2-step delete confirmation automatically
          if (g_del_confirm_idx >= 0 &&
              (g_del_confirm_idx != g_selected ||
               (now_input_ms - g_del_confirm_ms) >= 5500ull)) {
            g_del_confirm_idx = -1;
            longan::ui_set_delete_confirm(-1);
          }
          // Library screen: 4-column grid nav (L/R ±1, U/D ±4), A Play, X Exe, Y Rescan, Del Uninstall, B Home
          if (step_lt && !g_games.empty() && g_selected > 0) --g_selected;
          if (step_rt && !g_games.empty() && g_selected + 1 < (int)g_games.size()) ++g_selected;
          if (step_up && !g_games.empty()) g_selected = std::max(0, g_selected - 4);
          if (step_dn && !g_games.empty())
            g_selected = std::min((int)g_games.size() - 1, g_selected + 4);
          static bool del_k_was = false;
          const bool want_del = !km_lock && launcher_key(0x2E /*VK_DELETE*/);
          if (want_del && !del_k_was) {
            del_k_was = true;
            handle_delete_game(g_selected);
          } else if (!want_del) {
            del_k_was = false;
          }
          if (want_go && !a_btn) {
            a_btn = true;
            if (!g_games.empty()) {
              g_pending_launch = g_games[g_selected].name;
              longan::ui_sfx_launch();
              longan::ui_toast(L"Launching " + u82ws(g_games[g_selected].name) + L"…", 3.0f);
            }
          } else if (!want_go) a_btn = false;
          if (want_x && !x_btn) {
            x_btn = true;
            longan::ui_sfx_confirm();
            cycle_selected_exe(g_selected, 1);
          } else if (!want_x) x_btn = false;
          if (want_st && !y_btn) {
            y_btn = true;
            rescan_games();
            longan::ui_sfx_confirm();
            longan::ui_toast(L"Library rescanned: " + std::to_wstring(g_games.size()) + L" games", 2.5f);
          } else if (!want_st) y_btn = false;
          if (want_back && !b_btn) {
            b_btn = true;
            longan::ui_sfx_back();
            on_screen_enter(longan::UI_SHELF);
          } else if (!want_back) b_btn = false;
        } else if (!g_guest_running.load() && g_screen == longan::UI_TOOLS) {
          // Tools / Transfer / Logs screen: U/D select tool, A run tool,
          // X/Y scroll log up/down, B Home
          if (step_up) longan::ui_tools_nav_up();
          if (step_dn) longan::ui_tools_nav_down();
          if (want_go && !a_btn) {
            a_btn = true;
            run_console_tool(longan::ui_tools_selected());
          } else if (!want_go) a_btn = false;
          if (want_x && !x_btn) {
            x_btn = true;
            longan::ui_log_scroll(4);
          } else if (!want_x) x_btn = false;
          if (want_st && !y_btn) {
            y_btn = true;
            longan::ui_log_scroll(-4);
          } else if (!want_st) y_btn = false;
          if (want_back && !b_btn) {
            b_btn = true;
            on_screen_enter(longan::UI_SHELF);
          } else if (!want_back) b_btn = false;
        } else if (!g_guest_running.load() && g_screen == longan::UI_SETTINGS) {
          // settings screen nav: up/down select (auto-repeat), LR edit + persist,
          // A toggle, Y reset defaults, B Home
          if (step_up || step_dn) {
            g_set_selected = longan::ui_settings_step(step_up ? -1 : 1);
            longan::ui_set_settings(g_set_rows, g_set_count, g_set_selected);
          }
          if (want_x && !x_btn) {
            g_set_selected = longan::ui_settings_next_section();
            longan::ui_set_settings(g_set_rows, g_set_count, g_set_selected);
          }
          x_btn = want_x;
          if (step_lt && edit_setting_row(-1)) apply_settings_rows();
          if (step_rt && edit_setting_row(1)) apply_settings_rows();
          if (want_go && !a_btn) {
            a_btn = true;
            if (edit_setting_row(1)) apply_settings_rows();
          } else if (!want_go) a_btn = false;
          if (want_st && !y_btn) {
            y_btn = true;
            request_settings_reset();
          } else if (!want_st) y_btn = false;
          if (want_back && !b_btn) {
            b_btn = true;
            on_screen_enter(longan::UI_SHELF);
          } else if (!want_back) b_btn = false;
        } else if (!g_guest_running.load() && g_screen == longan::UI_STEAM) {
          // on-console Steam screen: up/down select title (auto-repeat), A download/play,
          // X refresh library, Y QR login, B Home
          if (step_up) longan::ui_steam_nav_up();
          if (step_dn) longan::ui_steam_nav_down();
          if (step_lt) longan::ui_steam_cycle_filter(-1);
          if (step_rt) longan::ui_steam_cycle_filter(1);
          const bool want_ref = want_x || (!km_lock && launcher_key('R'));
          if (want_ref && !x_btn) {
            x_btn = true;
            longan::steam_client_refresh_remote(
                []() { g_steam_needs_rescan.store(true); });
            longan::ui_toast(L"Refreshing Steam library…", 2.4f);
          } else if (!want_ref) {
            x_btn = false;
          }
          if (want_go && !a_btn) {
            a_btn = true;
            trigger_steam_item_action(longan::ui_steam_selected_app());
          } else if (!want_go) a_btn = false;
          if (want_st && !y_btn) {
            y_btn = true;
            steam_begin();
            longan::ui_toast(L"Generating Steam Mobile QR code…", 2.5f);
          } else if (!want_st) y_btn = false;
          if (want_back && !b_btn) {
            b_btn = true;
            on_screen_enter(longan::UI_SHELF);
          } else if (!want_back) b_btn = false;
        } else {
          a_btn = b_btn = x_btn = y_btn = false;
        }
        if (want_mm && !rs_btn) {
          rs_btn = true;
          g_mouse_mode = !g_mouse_mode;
        } else if (!want_mm) rs_btn = false;

        // gamepad-as-mouse + virtual KBM (KBM games without a real mouse/KB)
        if (pad_ok && g_mouse_mode) {
          mm_was_active = true;
          const auto pointer_settings = longan::settings_get();
          const int deadzone = 32767 * pointer_settings.input_deadzone / 100;
          const float dt = mm_last_motion_ms
              ? std::min<ULONGLONG>(now_input_ms - mm_last_motion_ms, 50) * 0.001f
              : 1.0f / 60.0f;
          mm_last_motion_ms = now_input_ms;
          const float speed = (0.25f + 0.015f * pointer_settings.mouse_speed) * 1400.0f;
          const auto pointer_delta = [&](int stick, float& fraction) {
            const int magnitude = std::max(0, std::abs(stick) - deadzone);
            const float motion = speed * dt * magnitude / (32767 - deadzone);
            fraction += stick < 0 ? -motion : motion;
            const int pixels = static_cast<int>(fraction);
            fraction -= pixels;
            return pixels;
          };
          const int dx = pointer_delta(xs.Gamepad.sThumbRX, mm_px);
          const int dy = pointer_delta(-static_cast<int>(xs.Gamepad.sThumbRY), mm_py);
          if (dx || dy) trt_input_mouse_delta_raw(dx, dy);

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
            const int wheel_direction = ((b & XINPUT_GAMEPAD_DPAD_UP) != 0) -
                                        ((b & XINPUT_GAMEPAD_DPAD_DOWN) != 0);
            if (wheel_direction &&
                (wheel_direction != mm_wheel_direction || now_input_ms >= mm_next_wheel_ms)) {
              trt_input_wheel(120 * wheel_direction);
              mm_next_wheel_ms = now_input_ms +
                  static_cast<ULONGLONG>(std::clamp(280 - 2 * pointer_settings.wheel_speed, 80, 278));
            }
            mm_wheel_direction = wheel_direction;
          }
        } else if (mm_was_active) {
          mm_was_active = false;
          mm_wheel_direction = 0;
          mm_next_wheel_ms = 0;
          mm_last_motion_ms = 0;
          mm_px = mm_py = 0.f;
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
        if (poll_external_storage()) rescan_games();
        // Rescan debounce + scope (item 4): uploads request a rescan per
        // file, and a full rescan re-walks every root + drive on the UI
        // thread — the prime wedge suspect. Pending rescans coalesce and
        // fire at most once per 2 s; when every pending change is for the
        // same game, only that game's dir is re-scanned. A full (root-only)
        // rescan still runs every 30 s so other changes surface.
        {
          std::string rescan_game;
          if (longan::webxfer_poll_rescan(&rescan_game)) {
            m_rescan_pending = true;
            if (rescan_game.empty()) {
              m_rescan_pending_full = true;
            } else if (m_rescan_pending_game.empty()) {
              m_rescan_pending_game = rescan_game;
            } else if (m_rescan_pending_game != rescan_game) {
              // two different games changed: scope is lost, rescan the root
              m_rescan_pending_full = true;
              m_rescan_pending_game.clear();
            }
          }
          const ULONGLONG now_ms = (ULONGLONG)GetTickCount64();
          if (m_next_full_rescan_ms == 0)
            m_next_full_rescan_ms = now_ms + 30000ull;
          if (m_rescan_pending && now_ms - g_last_rescan_ms >= 2000ull) {
            m_rescan_pending = false;
            if (m_rescan_pending_full) {
              m_rescan_pending_full = false;
              m_rescan_pending_game.clear();
              poll_external_storage(true);
              rescan_games();
            } else if (!m_rescan_pending_game.empty()) {
              const std::string game = m_rescan_pending_game;
              m_rescan_pending_game.clear();
              // A transfer just landed: arm the in-console autopilot for it.
              longan::host_ap_arm(game);
              longan::log_line(("host: autopilot armed for " + game).c_str());
              trt_engine_profile sprof;
              std::string gdir = g_local_state + "\\games\\" + game;
              trt_synthesize_engine(gdir.c_str(), nullptr, &sprof);
              rescan_one_game(game);
              longan::ui_toast(L"Transfer ready: " + u82ws(game), 3.5f);
            }
            // any rescan resets the periodic clock
            m_next_full_rescan_ms = now_ms + 30000ull;
          }
          if (now_ms >= m_next_full_rescan_ms) {
            m_next_full_rescan_ms = now_ms + 30000ull;
            rescan_games_root_only();
          }
        }
        std::string webname;
        if (longan::webxfer_poll_launch(&webname)) {
          g_pending_launch = webname;
          longan::host_ap_on_launch(webname);
          if (g_setup_active && g_setup_done) {
            g_setup_active = false;
            longan::ui_dismiss_first_run();
          }
          bool found = false;
          for (const auto& ge : g_games)
            if (_stricmp(ge.name.c_str(), webname.c_str()) == 0) { found = true; break; }
          if (!found) rescan_games();
        }
        if (!g_pending_launch.empty()) {
          const std::string want = g_pending_launch;
          g_pending_launch.clear();
          bool shelf_hit = false;
          for (const auto& ge : g_games)
            if (_stricmp(ge.name.c_str(), want.c_str()) == 0) { shelf_hit = true; break; }
          if (!shelf_hit) {
            // console reality: the shelf's FindFirstFileA scan can come up
            // empty while the web dashboard's enumeration lists the game —
            // fall back to the webxfer's own list so a web launch NEVER
            // drops silently
            char wexe[1024] = {};
            std::string wdir;
            if (longan::webxfer_find_game(want.c_str(), wexe, sizeof(wexe), &wdir)) {
              std::string dir_rel = want;
              for (char& c : dir_rel) if (c == '/') c = '\\';
              game_entry fge;
              fge.name = want;
              fge.exe = wexe;
              fge.exes.push_back(wexe);
              fge.dir = wdir.empty()
                  ? g_games_dir + std::string(1, static_cast<char>(92)) + dir_rel
                  : wdir;
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
              longan::webxfer_set_launch_result(want, "not found");
              longan::host_ap_on_result(want, "not found");
            }
          }
          for (const auto& ge : g_games) {
            if (_stricmp(ge.name.c_str(), want.c_str()) == 0) {
              // The previous guest thread may still be unwinding after
              // run_game returned (the running flag clears in the lambda's
              // tail, so this join normally returns at once) — but never
              // block the UI thread forever on it: 5 s, then force-kill.
              if (g_guest_thread.joinable()) {
                if (::WaitForSingleObject(g_guest_thread.native_handle(),
                                          5000) == WAIT_OBJECT_0) {
                  g_guest_thread.join();
                } else {
                  force_kill_guest("join timeout");
                }
              }
              set_guest_status("running " + ge.name + "…");
              const std::string input_mode = ge.input_mode.empty() || ge.input_mode == "auto"
                  ? longan::settings_get().default_input : ge.input_mode;
              g_auto_pad_mouse.store(false);
              g_mouse_mode = input_mode == "pad2kbm";
              if (input_mode == "auto" && !g_host_mouse_present.load()) {
                // Auto mode with no hardware pointer available: launch with the
                // pad-driven cursor rather than a title that polls the mouse and
                // never receives a single event.
                g_mouse_mode = true;
                g_auto_pad_mouse.store(true);
                longan::log_line("input: auto input mode, no hardware pointer -> "
                                 "pad-as-mouse ON for this launch");
              }
              trt_input_release_all();
              // Keep the visible console cursor active for all games unless
              // the guest explicitly pins it to a 1x1 mouselook trap.
              trt_input_reset_cursor_mode(true);
              longan::webxfer_set_running_game(want);
              longan::webxfer_set_launch_result(want, "started");
              longan::host_ap_on_result(want, "started");
              // The webxfer machine matched the log and may have applied a
              // repair; mirror its verdict into the host-side state.
              {
                std::string ap_game, ap_result;
                int ap_attempt = 0;
                longan::webxfer_autopilot_state(&ap_game, &ap_attempt,
                                                &ap_result);
                if (ap_game == want && ap_result == "fix")
                  longan::host_ap_on_fix(ap_attempt);
                else if (ap_game == want &&
                         (ap_result == "no-fix" ||
                          ap_result == "attempt-limit"))
                  longan::host_ap_on_no_fix();
              }
              {
                char ap_msg[192];
                snprintf(ap_msg, sizeof(ap_msg),
                         "host: autopilot status=%s attempt=%d game=%s",
                         g_host_ap.status.c_str(), g_host_ap.attempt,
                         g_host_ap.game.c_str());
                longan::log_line(ap_msg);
              }
              g_running_game_name = want;
              g_guest_running.store(true);
              const std::string exe = ge.exe;
              const std::string gdir = ge.dir;
              const std::string img = ge.image;
              g_guest_thread = std::thread([want, exe, gdir, img]() {
                // the guest main thread's stack reserve is VA for its
                // lifetime (1 MB Windows default) — charge it here, where
                // the thread is created, and discharge it in the tail
                // (a force-killed thread never runs the tail, but the
                // force-kill path resets the whole budget instead)
                const bool stack_charged =
                    trt_mem_budget_charge_va(1024ull * 1024ull) == 0;
                const auto gr = run_game(want, exe, gdir, img);
                char line[192];
                snprintf(line, sizeof(line), "launch %s: rc=%s (%s)",
                         want.c_str(), trt_errstr(gr.map_rc), gr.detail);
                longan::log_line(line);
                if (gr.map_rc == TRT_OK) {
                  if (gr.detail[0]) {
                    set_guest_status(want + ": " + std::string(gr.detail));
                  } else {
                    set_guest_status(want + ": Finished");
                  }
                } else {
                  set_guest_status(want + ": " +
                                   (gr.detail[0] ? gr.detail : trt_errstr(gr.map_rc)));
                }
                if (stack_charged)
                  trt_mem_budget_discharge_va(1024ull * 1024ull);
                g_running_game_name.clear();
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
        // Quick menu navigation: D-Pad + Left Stick + Keyboard (Up/Down/Enter/Esc) + Mouse clicks
        static bool qm_up = false, qm_dn = false, qm_go = false, qm_back = false;
        XINPUT_STATE qm_xs{};
        ZeroMemory(&qm_xs, sizeof(qm_xs));
        const bool qm_pad = (XInputGetState(0, &qm_xs) == ERROR_SUCCESS);
        const u16 qm_b = qm_pad ? qm_xs.Gamepad.wButtons : 0;
        const bool want_up = (qm_b & XINPUT_GAMEPAD_DPAD_UP) != 0 ||
                             (qm_pad && qm_xs.Gamepad.sThumbLY > 16000) ||
                             launcher_key(0x26 /*VK_UP*/) || launcher_key('W');
        const bool want_dn = (qm_b & XINPUT_GAMEPAD_DPAD_DOWN) != 0 ||
                             (qm_pad && qm_xs.Gamepad.sThumbLY < -16000) ||
                             launcher_key(0x28 /*VK_DOWN*/) || launcher_key('S');
        const bool want_go = (qm_b & XINPUT_GAMEPAD_A) != 0 ||
                             launcher_key(0x0D /*VK_RETURN*/) ||
                             launcher_key(0x20 /*VK_SPACE*/);
        const bool want_bk = (qm_b & XINPUT_GAMEPAD_B) != 0 ||
                             launcher_key(0x1B /*VK_ESCAPE*/);
        const bool pad_vm =
            (qm_b & XINPUT_GAMEPAD_BACK) && (qm_b & XINPUT_GAMEPAD_START);
        if (want_up && !qm_up) {
          qm_up = true;
          longan::ui_quick_menu_nav_up();
        } else if (!want_up) qm_up = false;
        if (want_dn && !qm_dn) {
          qm_dn = true;
          longan::ui_quick_menu_nav_down();
        } else if (!want_dn) qm_dn = false;
        if (want_go && !qm_go) {
          qm_go = true;
          execute_quick_menu_action(longan::ui_quick_menu_selected());
        } else if (!want_go) qm_go = false;
        if (want_bk && !qm_back) {
          qm_back = true;
          g_quick_menu_open = false;
          longan::ui_set_quick_menu(false);
        } else if (!want_bk) qm_back = false;
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
        // sync live Steam persona, friends & unlocked achievements + render overlay
        sync_steam_to_ui();
        longan::swapchain_restore_d3d11();
        longan::ui_set_screen(longan::UI_QUICKMENU);
        longan::ui_render();
        longan::swapchain_present();
        Sleep(16);  // ~60fps for menu
        continue;
      }
      longan::swapchain_restore_d3d11();
      longan::swapchain_restore_launcher_format();

      if (g_steam_needs_rescan.exchange(false)) {
        rescan_games();
        longan::steam_client_rescan_local();
      }
      sync_steam_to_ui();

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
      {
        uint64_t up_done = 0, up_total = 0;
        std::string up_label;
        longan::webxfer_upload_progress(&up_done, &up_total, &up_label);
        longan::ui_set_upload_progress(up_done, up_total, u82ws(up_label));
      }
      if (g_screen == longan::UI_STEAM && g_steam_busy && !g_steam_qr.empty()) {
        longan::ui_set_qr(g_steam_qr);
      } else {
        longan::ui_set_qr(transfer_url);
      }
      if (!g_setup_active) {
        std::vector<longan::ui_game> uig;
        uig.reserve(g_games.size());
        for (const auto& ge : g_games) {
          std::wstring disp = u82ws(ge.exe);
          if (!ge.image.empty()) disp = L"[disc] " + disp;
          if (ge.exes.size() > 1) {
            disp = L"[" + std::to_wstring(ge.exe_idx + 1) + L"/" +
                   std::to_wstring(ge.exes.size()) + L"] " + disp;
          }
          uig.push_back({u82ws(ge.name), disp, u82ws(ge.input_mode),
                         infer_engine_label(ge), ge.name});
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
        if (!g_games.empty() && g_selected < (int)g_games.size()) {
          lines.push_back(u82ws(g_games[g_selected].compat));
        }
        longan::ui_set_lines(lines);
      }
      if (g_screen == longan::UI_TOOLS && !g_setup_active) {
        // live log tail on the Tools/Transfer screen: pull the in-memory ring
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
      const float t_sec =
          static_cast<float>(GetTickCount64() - g_boot_ms) * 0.001f;
      longan::swapchain_render_frame(t_sec);
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

// Startup crash guard: top-level Vectored Exception Handler that captures any
// fault before or during window/Direct3D initialization and writes a post-mortem
// log into tiramisu_startup_crash.log in LocalState (or working dir).
static LONG WINAPI startup_veh_filter(EXCEPTION_POINTERS* ep) {
  if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
  const DWORD code = ep->ExceptionRecord->ExceptionCode;
  if (code == 0x406D1388 /* SetThreadName */ ||
      code == 0x000006BA /* RPC_S_SERVER_UNAVAILABLE */ ||
      code == 0x000006BE /* RPC_S_CALL_FAILED */ ||
      code == 0x80000003 /* STATUS_BREAKPOINT */) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  if (code != STATUS_ACCESS_VIOLATION &&
      code != STATUS_ILLEGAL_INSTRUCTION &&
      code != STATUS_DATATYPE_MISALIGNMENT &&
      code != STATUS_IN_PAGE_ERROR &&
      code != STATUS_STACK_OVERFLOW) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  // The unwind itself can fault if the stack is damaged. Avoid
  // recursively entering the crash logger in that case.
  static std::atomic_flag recording = ATOMIC_FLAG_INIT;
  if (recording.test_and_set(std::memory_order_acquire))
    return EXCEPTION_CONTINUE_SEARCH;

  char buf[512];
  if (code == 0xC000001Dul) {
    snprintf(buf, sizeof(buf),
             "FIRST CHANCE EXCEPTION: STATUS_ILLEGAL_INSTRUCTION (0xC000001D) at addr=%p\n",
             ep->ExceptionRecord->ExceptionAddress);
  } else {
    snprintf(buf, sizeof(buf),
             "FIRST CHANCE EXCEPTION: code=0x%08lx addr=%p\n",
             (unsigned long)code, ep->ExceptionRecord->ExceptionAddress);
  }

  std::string crash_path = g_local_state.empty() ? "tiramisu_startup_crash.log"
                                                 : (g_local_state + "\\tiramisu_startup_crash.log");
  FILE* fp = fopen(crash_path.c_str(), "a");
  if (fp) {
    fputs(buf, fp);
    if (ep->ContextRecord) {
      char gprs[256];
      snprintf(gprs, sizeof(gprs),
               "GPRs: rip=%llx rsp=%llx rax=%llx rcx=%llx rdx=%llx\n",
               (unsigned long long)ep->ContextRecord->Rip,
               (unsigned long long)ep->ContextRecord->Rsp,
               (unsigned long long)ep->ContextRecord->Rax,
               (unsigned long long)ep->ContextRecord->Rcx,
               (unsigned long long)ep->ContextRecord->Rdx);
      fputs(gprs, fp);
      fprintf(fp, "  THREAD: tid=%lu\n", (unsigned long)::GetCurrentThreadId());
      if (::GetCurrentThreadId() == g_guest_tid) {
        void* const active_tls = reinterpret_cast<void*>(__readgsqword(0x58));
        void* const guest_tls = trt_guest_tls_array();
        fprintf(fp, "  TLS: active=%p guest=%p\n", active_tls, guest_tls);
      }

      DWORD64 frames[12]{};
      const int count = capture_fault_stack(ep->ContextRecord, frames, 12);
      for (int i = 0; i < count; ++i) {
        const auto pc = static_cast<uintptr_t>(frames[i]);
        MEMORY_BASIC_INFORMATION page{};
        ::VirtualQuery(reinterpret_cast<void*>(pc), &page, sizeof(page));
        fprintf(fp, "  FRAME %d: pc=%p allocation=%p\n", i,
                reinterpret_cast<void*>(pc), page.AllocationBase);
      }

      if (ep->ExceptionRecord->NumberParameters >= 2) {
        char access[192];
        snprintf(access, sizeof(access),
                 "  EXCEPTION-ACCESS: operation=%llu target=0x%llx\n",
                 (unsigned long long)ep->ExceptionRecord->ExceptionInformation[0],
                 (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
        fputs(access, fp);
      }
      // The exception callback runs on the faulting thread. Record its
      // readable stack words so a post-mortem identifies the native caller,
      // rather than only the ntdll faulting instruction and registers.
      const uintptr_t sp = (uintptr_t)ep->ContextRecord->Rsp;
      MEMORY_BASIC_INFORMATION mbi{};
      if (::VirtualQuery((const void*)sp, &mbi, sizeof(mbi)) != 0 &&
          mbi.State == MEM_COMMIT &&
          !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
        const uintptr_t base = (uintptr_t)mbi.BaseAddress;
        const uintptr_t end = base + mbi.RegionSize;
        uintptr_t words[12]{};
        if (sp >= base && sp <= end && sizeof(words) <= end - sp) {
          memcpy(words, (const void*)sp, sizeof(words));
          char stack[512];
          snprintf(stack, sizeof(stack),
                   "  STACK: rsp+00=%llx rsp+08=%llx rsp+10=%llx rsp+18=%llx rsp+20=%llx rsp+28=%llx rsp+30=%llx rsp+38=%llx rsp+40=%llx rsp+48=%llx rsp+50=%llx rsp+58=%llx\n",
                   (unsigned long long)words[0], (unsigned long long)words[1],
                   (unsigned long long)words[2], (unsigned long long)words[3],
                   (unsigned long long)words[4], (unsigned long long)words[5],
                   (unsigned long long)words[6], (unsigned long long)words[7],
                   (unsigned long long)words[8], (unsigned long long)words[9],
                   (unsigned long long)words[10], (unsigned long long)words[11]);
          fputs(stack, fp);
        }
      }
    }
    fflush(fp);
    fclose(fp);
  }
  longan::log_line(buf);
  recording.clear(std::memory_order_release);
  return EXCEPTION_CONTINUE_SEARCH;
}

// .211 DIAG: host-side unhandled-exception filter. The guest faults
// are caught by guest_seh_filter (call_guest_seh), so anything
// reaching here is a HOST crash (host thread, D3D/GameInput callback,
// COM) -- previously silent (process dies, no fault line, no dump).
// log_line flushes, so the line survives the kill. Returns
// CONTINUE_SEARCH so the OS terminates normally after logging.
static LONG WINAPI host_unhandled_filter(EXCEPTION_POINTERS* ep) {
  if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
  checkpoint_outcome(longan::launch_outcome::host_fault, ep->ExceptionRecord->ExceptionCode);
  char line[256];
  if (ep->ExceptionRecord && ep->ExceptionRecord->ExceptionCode == 0xC000001Dul) {
    snprintf(line, sizeof(line), "HOST UNHANDLED: STATUS_ILLEGAL_INSTRUCTION (0xC000001D) at addr=%p (unsupported opcode/AVX2 on Jaguar)",
             ep->ExceptionRecord->ExceptionAddress);
  } else {
    snprintf(line, sizeof(line), "HOST UNHANDLED: code=0x%08lx addr=%p",
             (unsigned long)ep->ExceptionRecord->ExceptionCode,
             ep->ExceptionRecord->ExceptionAddress);
  }
  longan::log_line(line);
  if (ep->ContextRecord) {
    const CONTEXT& c = *ep->ContextRecord;
    char g[256];
    snprintf(g, sizeof(g),
             "  HOST GPRs: rip=%llx rsp=%llx rax=%llx rcx=%llx rdx=%llx",
             (unsigned long long)c.Rip, (unsigned long long)c.Rsp,
             (unsigned long long)c.Rax, (unsigned long long)c.Rcx,
             (unsigned long long)c.Rdx);
    longan::log_line(g);
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

// .212 DIAG: host-side exit-path handlers. The guest real CRT (ucrtbase is
// TRT_ROUTE_SYSTEM) shares the process CRT with the host, so a guest exit()
// via the real CRT runs the host atexit list, a guest abort() raises the
// process SIGABRT, and an unhandled C++ exception calls terminate(). These
// log (flushed) so a silent death via any of those paths becomes visible.
static void host_atexit_handler() {
  checkpoint_outcome(longan::launch_outcome::host_exit, 0);
  longan::log_line("host: atexit (clean process exit via real CRT exit())");
}
static void host_terminate_handler() {
  longan::log_line("host: terminate() called (unhandled C++ exception)");
  std::abort();
}
static void host_sigabrt_handler(int) {
  checkpoint_outcome(longan::launch_outcome::host_abort, 3);
  longan::log_line("host: SIGABRT (abort() called via real CRT)");
  std::_Exit(3);
}

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  AddVectoredExceptionHandler(1, startup_veh_filter);
  SetUnhandledExceptionFilter(host_unhandled_filter);
  std::atexit(host_atexit_handler);
  std::set_terminate(host_terminate_handler);
  std::signal(SIGABRT, host_sigabrt_handler);
  init_apartment(apartment_type::multi_threaded);

  // Early disk logging: open log immediately on entry before COM/DirectX initialization
  const std::string install_early = package_install_path();
  longan::log_open(install_early);

  // Auto-detect Xbox console hardware generation early
  const auto cons_info = longan::get_detected_console_info();
  {
    char cmsg[512];
    snprintf(cmsg, sizeof(cmsg),
             "console_detect: detected %s (%s, family=%s) | CPU: %s %d cores @ %.2f GHz (AVX2=%d) | GPU: %s (%.2f TF) | RAM: %d MB (Dev Mode budget %d MB) | ESRAM: %d MB",
             cons_info.generation_name, cons_info.codename, cons_info.family_id,
             cons_info.cpu_arch, cons_info.cpu_cores, cons_info.cpu_clock_ghz,
             cons_info.has_avx2 ? 1 : 0, cons_info.gpu_arch, cons_info.gpu_tflops,
             cons_info.total_ram_mb, cons_info.dev_mode_game_budget_mb,
             cons_info.esram_size_mb);
    longan::log_line(cmsg);
  }

  // Console lifetime: the app is suspended when the player leaves it and
  // resumed when they come back. Release held inputs on both edges so a game
  // never resumes with a phantom held button, and log each edge so a freeze
  // after resume has a timestamp to line up with. CoreApplication raises
  // these statically; Current() belongs to Application, not to it.
  try {
    winrt::Windows::ApplicationModel::Core::CoreApplication::Suspending(
        [](const winrt::Windows::Foundation::IInspectable&,
           const winrt::Windows::ApplicationModel::SuspendingEventArgs& e) {
          auto deferral = e.SuspendingOperation().GetDeferral();
          trt_input_release_all();
          {
            char lc[256];
            snprintf(lc, sizeof(lc), "lifecycle: suspending - inputs released game=%s",
                     g_running_game_name.empty() ? "(none)" : g_running_game_name.c_str());
            longan::log_line(lc);
          }
          deferral.Complete();
        });
    winrt::Windows::ApplicationModel::Core::CoreApplication::Resuming(
        [](auto&&, auto&&) {
          trt_input_release_all();  // devices are re-polled on the way back
          {
            char lc[256];
            snprintf(lc, sizeof(lc), "lifecycle: resumed - inputs re-polled game=%s",
                     g_running_game_name.empty() ? "(none)" : g_running_game_name.c_str());
            longan::log_line(lc);
          }
        });
  } catch (...) {
    longan::log_line("lifecycle: no CoreApplication suspend hook on this host");
  }
  CoreApplication::Run(make<tiramisu_host>());
  return 0;
}
