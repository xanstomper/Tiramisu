// tiramisu/src/trt/bridge_user32_gap.cpp
// Complete user32.dll desktop compatibility surface for Xbox Series X|S UWP.
// Implements monitor/display enumeration (1920x1080@60Hz primary monitor),
// window long slots (distinct per-offset table so GWLP_USERDATA -21 never
// collides with GWL_STYLE -16 / GWL_EXSTYLE -20), window geometry, raw input
// device list, clipboard, keyboard layout/translation, cursor/icon records,
// window properties, touch/pointer stubs, menus, paint/rect helpers, DPI
// awareness, and ANSI/Wide wrappers.
#include "bridge_gdi.h"
#include "bridge_user32.h"
#include "gdi_raster.h"
#include "thunk.h"
#include "trt.h"
#include "pe.h"
#include "input.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <unordered_map>

namespace u32gap {
void line(const char* s) { trt_core_log(s); }
}
#define LOG(s_) u32gap::line(s_)

namespace {

extern "C" void TRT_STDCALL trt_SetLastError(u32 e);
extern "C" u16 TRT_STDCALL trt_RegisterClassExW(const void* wcx);
extern "C" void* TRT_STDCALL trt_CreateWindowExW(u32 exstyle,
    const wchar_t* cls, const wchar_t* title, u32 style, int x, int y,
    int w, int h, void* parent, void* menu, void* inst, void* param);
extern "C" i64 TRT_STDCALL trt_DefWindowProcW(void* hwnd, u32 msg, u64 wp, i64 lp);
extern "C" void* TRT_STDCALL trt_GetActiveWindow(void);
extern "C" int TRT_STDCALL trt_GetMessageW(void* out, void* hwnd, u32 min, u32 max);
extern "C" int TRT_STDCALL trt_PeekMessageW(void* out, void* hwnd, u32 min, u32 max, u32 remove);
extern "C" int TRT_STDCALL trt_PostMessageW(void* hwnd, u32 msg, u64 wp, i64 lp);
extern "C" i64 TRT_STDCALL trt_DispatchMessageW(const void* m);
extern "C" i64 TRT_STDCALL trt_SendMessageW(void* hwnd, u32 msg, u64 wp, i64 lp);
extern "C" void* TRT_STDCALL trt_GetDC(void* hwnd);
extern "C" int trt_user32_client_size(void* hwnd, i32* w, i32* h);
extern "C" int trt_user32_window_rect(void* hwnd, i32* rect);
extern "C" int TRT_STDCALL trt_SetWindowPos(void* hwnd, void* after, i32 x,
                                          i32 y, i32 cx, i32 cy, u32 flags);
extern "C" void trt_user32_invalidate(void* hwnd, const i32* rect);
extern "C" int trt_user32_update_rect(void* hwnd, i32* rect);
extern "C" void trt_user32_validate(void* hwnd, const i32* rect);
extern "C" int TRT_STDCALL trt_ReleaseDC(void* hwnd, void* hdc);
extern "C" int TRT_STDCALL trt_GetSystemMetrics(int idx);
extern "C" u32 TRT_STDCALL trt_MapVirtualKeyW(u32 code, u32 type);
extern "C" u32 trt_vk_to_char_ex(u32 vk, const u8* ks);
extern "C" i16 trt_char_to_vk_scan(u16 ch);
extern "C" u32 TRT_STDCALL trt_WaitForMultipleObjects(u32 count,
    const u64* handles, int wait_all, u32 ms);

// Per-window GWL_* / GWLP_* slot storage indexed directly by (k_slot_base + idx)
// so GWLP_USERDATA (-21), GWL_EXSTYLE (-20), GWL_STYLE (-16), GWLP_ID (-12),
// GWLP_HWNDPARENT (-8), GWLP_HINSTANCE (-6), GWLP_WNDPROC (-4), and positive
// cbWndExtra offsets each have a distinct 64-bit slot.
constexpr int k_slot_base = 32;
constexpr int k_slot_count = 96;
void** slots_for(u64 hwnd) {
  static std::mutex g_slots_mu;
  static std::unordered_map<u64, std::array<void*, k_slot_count>> g_slots;
  std::lock_guard<std::mutex> lk(g_slots_mu);
  return g_slots[hwnd].data();
}

struct rect_i32 { i32 l, t, r, b; };
struct point_i32 { i32 x, y; };

constexpr u64 k_primary_hmonitor = 0x00050001ULL;

// Property table for SetProp/GetProp/RemoveProp
struct win_prop {
  u64 hwnd;
  char name[64];
  void* data;
  bool used;
};
win_prop g_props[64];

}  // namespace

extern "C" {

// ---- Class registration & queries ----
u16 TRT_STDCALL trt_RegisterClassW(const void* wcx) {
  struct wcxw { u32 cbSize; u32 style; void* proc; i32 cls_x; i32 win_x;
                void* inst; void* icon; void* cursor; void* brush;
                const wchar_t* menu; const wchar_t* name; };
  struct wcw { u32 style; void* proc; i32 cls_x; i32 win_x; void* inst;
               void* icon; void* cursor; void* brush; const wchar_t* menu;
               const wchar_t* name; };
  if (!wcx) { trt_SetLastError(87); return 0; }
  const wcw* c = (const wcw*)wcx;
  wcxw ex{};
  ex.cbSize = sizeof(wcxw);
  ex.style = c->style; ex.proc = c->proc; ex.cls_x = c->cls_x;
  ex.win_x = c->win_x; ex.inst = c->inst; ex.icon = c->icon;
  ex.cursor = c->cursor; ex.brush = c->brush; ex.menu = c->menu;
  ex.name = c->name;
  return trt_RegisterClassExW(&ex);
}

u16 TRT_STDCALL trt_RegisterClassExA(const void* wcx) {
  struct wcxa { u32 cbSize; u32 style; void* proc; i32 cls_x; i32 win_x;
                void* inst; void* icon; void* cursor; void* brush;
                const char* menu; const char* name; void* icon_sm; };
  struct wcxw { u32 cbSize; u32 style; void* proc; i32 cls_x; i32 win_x;
                void* inst; void* icon; void* cursor; void* brush;
                const wchar_t* menu; const wchar_t* name; };
  if (!wcx) return 0;
  const wcxa* ca = (const wcxa*)wcx;
  static wchar_t wname[64];
  memset(wname, 0, sizeof(wname));
  if (ca->name && ((uintptr_t)ca->name >> 16) != 0) {
    for (int i = 0; i < 63 && ca->name[i]; ++i) wname[i] = (wchar_t)(u8)ca->name[i];
  } else {
    wname[0] = 'A'; wname[1] = 0;
  }
  wcxw cw{};
  cw.cbSize = sizeof(wcxw);
  cw.style = ca->style; cw.proc = ca->proc; cw.cls_x = ca->cls_x;
  cw.win_x = ca->win_x; cw.inst = ca->inst; cw.icon = ca->icon;
  cw.cursor = ca->cursor; cw.brush = ca->brush; cw.name = wname;
  return trt_RegisterClassExW(&cw);
}

u16 TRT_STDCALL trt_RegisterClassA(const void* wc) {
  struct wca { u32 style; void* proc; i32 cls_x; i32 win_x; void* inst;
               void* icon; void* cursor; void* brush; const char* menu;
               const char* name; };
  struct wcxa { u32 cbSize; u32 style; void* proc; i32 cls_x; i32 win_x;
                void* inst; void* icon; void* cursor; void* brush;
                const char* menu; const char* name; void* icon_sm; };
  if (!wc) return 0;
  const wca* c = (const wca*)wc;
  wcxa ex{};
  ex.cbSize = sizeof(wcxa);
  ex.style = c->style; ex.proc = c->proc; ex.cls_x = c->cls_x;
  ex.win_x = c->win_x; ex.inst = c->inst; ex.icon = c->icon;
  ex.cursor = c->cursor; ex.brush = c->brush; ex.menu = c->menu;
  ex.name = c->name;
  return trt_RegisterClassExA(&ex);
}

int TRT_STDCALL trt_UnregisterClassW(const void* cls, void* inst) {
  (void)cls; (void)inst;
  return 1;
}
int TRT_STDCALL trt_UnregisterClassA(const void* cls, void* inst) {
  (void)cls; (void)inst;
  return 1;
}
int TRT_STDCALL trt_GetClassInfoExW(void* inst, const void* cls, void* out) {
  (void)inst; (void)cls;
  if (out) memset(out, 0, 80);
  return 1;
}
int TRT_STDCALL trt_GetClassInfoExA(void* inst, const void* cls, void* out) {
  (void)inst; (void)cls;
  if (out) memset(out, 0, 80);
  return 1;
}
int TRT_STDCALL trt_GetClassInfoW(void* inst, const void* cls, void* out) {
  (void)inst; (void)cls;
  if (out) memset(out, 0, 72);
  return 1;
}
int TRT_STDCALL trt_GetClassInfoA(void* inst, const void* cls, void* out) {
  (void)inst; (void)cls;
  if (out) memset(out, 0, 72);
  return 1;
}
int TRT_STDCALL trt_GetClassNameW(void* hwnd, u16* buf, int max_cnt) {
  (void)hwnd;
  if (!buf || max_cnt <= 0) return 0;
  static const char k_cls[] = "TiramisuWndClass";
  int i = 0;
  for (; k_cls[i] && i < max_cnt - 1; ++i) buf[i] = (u16)k_cls[i];
  buf[i] = 0;
  return i;
}
int TRT_STDCALL trt_GetClassNameA(void* hwnd, char* buf, int max_cnt) {
  (void)hwnd;
  if (!buf || max_cnt <= 0) return 0;
  return snprintf(buf, (size_t)max_cnt, "TiramisuWndClass");
}
u64 TRT_STDCALL trt_GetClassLongPtrW(void* hwnd, int idx) {
  (void)hwnd; (void)idx;
  return 0;
}
u64 TRT_STDCALL trt_GetClassLongPtrA(void* hwnd, int idx) {
  (void)hwnd; (void)idx;
  return 0;
}
u64 TRT_STDCALL trt_SetClassLongPtrW(void* hwnd, int idx, u64 val) {
  (void)hwnd; (void)idx; (void)val;
  return 0;
}
u64 TRT_STDCALL trt_SetClassLongPtrA(void* hwnd, int idx, u64 val) {
  (void)hwnd; (void)idx; (void)val;
  return 0;
}
u32 TRT_STDCALL trt_GetClassLongW(void* hwnd, int idx) {
  (void)hwnd; (void)idx;
  return 0;
}
u32 TRT_STDCALL trt_GetClassLongA(void* hwnd, int idx) {
  (void)hwnd; (void)idx;
  return 0;
}
u32 TRT_STDCALL trt_SetClassLongW(void* hwnd, int idx, u32 val) {
  (void)hwnd; (void)idx; (void)val;
  return 0;
}
u32 TRT_STDCALL trt_SetClassLongA(void* hwnd, int idx, u32 val) {
  (void)hwnd; (void)idx; (void)val;
  return 0;
}

void* TRT_STDCALL trt_CreateWindowExA(u32 exstyle, const char* cls,
                                      const char* title, u32 style,
                                      int x, int y, int w, int h,
                                      void* parent, void* menu, void* inst,
                                      void* param) {
  wchar_t wcls[64] = {}, wtitle[64] = {};
  if (cls && ((uintptr_t)cls >> 16) != 0)
    for (int i = 0; i < 63 && cls[i]; ++i) wcls[i] = (wchar_t)(u8)cls[i];
  if (title)
    for (int i = 0; i < 63 && title[i]; ++i) wtitle[i] = (wchar_t)(u8)title[i];
  return trt_CreateWindowExW(exstyle, wcls, wtitle, style, x, y, w, h,
                             parent, menu, inst, param);
}

// ---- Window Long / LongPtr ----
i64 TRT_STDCALL trt_SetWindowLongPtrW(u64 hwnd, int idx, i64 value) {
  if (idx == -4) return trt_user32_window_proc(hwnd, &value);
  void** slots = slots_for(hwnd);
  const int slot = k_slot_base + idx;
  if (slot < 0 || slot >= k_slot_count) return 0;
  const i64 old = (i64)(intptr_t)slots[slot];
  slots[slot] = (void*)(uintptr_t)value;
  return old;
}

i64 TRT_STDCALL trt_GetWindowLongPtrW(u64 hwnd, int idx) {
  if (idx == -4) return trt_user32_window_proc(hwnd, nullptr);
  void** slots = slots_for(hwnd);
  const int slot = k_slot_base + idx;
  if (slot < 0 || slot >= k_slot_count) return 0;
  return (i64)(intptr_t)slots[slot];
}

i64 TRT_STDCALL trt_SetWindowLongPtrA(u64 hwnd, int idx, i64 value) {
  return trt_SetWindowLongPtrW(hwnd, idx, value);
}
i64 TRT_STDCALL trt_GetWindowLongPtrA(u64 hwnd, int idx) {
  return trt_GetWindowLongPtrW(hwnd, idx);
}
i32 TRT_STDCALL trt_SetWindowLongW(u64 hwnd, int idx, i32 value) {
  return (i32)trt_SetWindowLongPtrW(hwnd, idx, (i64)value);
}
i32 TRT_STDCALL trt_GetWindowLongW(u64 hwnd, int idx) {
  return (i32)trt_GetWindowLongPtrW(hwnd, idx);
}
i32 TRT_STDCALL trt_SetWindowLongA(u64 hwnd, int idx, i32 value) {
  return (i32)trt_SetWindowLongPtrW(hwnd, idx, (i64)value);
}

// ---- Window Text ----
int TRT_STDCALL trt_GetWindowTextW(u64 hwnd, u16* str, int maxCount) {
  return str && maxCount > 0 ? trt_user32_window_title(hwnd, str, maxCount) : 0;
}
int TRT_STDCALL trt_GetWindowTextA(u64 hwnd, char* str, int maxCount) {
  if (!str || maxCount <= 0) return 0;
  u16 title[64];
  const int length = trt_user32_window_title(hwnd, title, maxCount < 64 ? maxCount : 64);
  for (int i = 0; i < length; ++i) str[i] = static_cast<char>(title[i]);
  str[length] = 0;
  return length;
}
int TRT_STDCALL trt_GetWindowTextLengthW(u64 hwnd) {
  return trt_user32_window_title(hwnd, nullptr, 0);
}
int TRT_STDCALL trt_GetWindowTextLengthA(u64 hwnd) {
  return trt_GetWindowTextLengthW(hwnd);
}
extern i64 TRT_STDCALL trt_SetWindowTextW(void* hwnd, const u16* text);
int TRT_STDCALL trt_SetWindowTextA(void* hwnd, const char* text) {
  u16 title[64] = {};
  for (int i = 0; text && i < 63 && text[i]; ++i)
    title[i] = static_cast<u8>(text[i]);
  return static_cast<int>(trt_SetWindowTextW(hwnd, title));
}

// ---- Monitor & Display Enumeration (1920x1080@60Hz Primary Monitor) ----
void* TRT_STDCALL trt_MonitorFromWindow(void* hwnd, u32 flags) {
  (void)hwnd; (void)flags;
  return (void*)(uintptr_t)k_primary_hmonitor;
}
void* TRT_STDCALL trt_MonitorFromRect(const void* lprc, u32 flags) {
  (void)lprc; (void)flags;
  return (void*)(uintptr_t)k_primary_hmonitor;
}
void* TRT_STDCALL trt_MonitorFromPoint(u64 pt, u32 flags) {
  (void)pt; (void)flags;
  return (void*)(uintptr_t)k_primary_hmonitor;
}

// MONITORINFO (40 bytes), MONITORINFOEXW (104 bytes), MONITORINFOEXA (72 bytes)
int TRT_STDCALL trt_GetMonitorInfoW(void* hmon, void* lpmi) {
  (void)hmon;
  if (!lpmi) return 0;
  u32 cb = *(u32*)lpmi;
  if (cb < 40) return 0;
  int w = 1920, h = 1080;
  trt_display_size(&w, &h);
  if (w <= 0) w = 1920;
  if (h <= 0) h = 1080;
  struct mi_base {
    u32 cbSize;
    rect_i32 rcMonitor;
    rect_i32 rcWork;
    u32 dwFlags;
  };
  mi_base* m = (mi_base*)lpmi;
  m->rcMonitor = {0, 0, w, h};
  m->rcWork = {0, 0, w, h};
  m->dwFlags = 1;  // MONITORINFOF_PRIMARY
  if (cb >= 104) {
    u16* dev = (u16*)((u8*)lpmi + 40);
    memset(dev, 0, 64);
    static const char k_name[] = "\\\\.\\DISPLAY1";
    for (int i = 0; k_name[i]; ++i) dev[i] = (u16)k_name[i];
  }
  return 1;
}

int TRT_STDCALL trt_GetMonitorInfoA(void* hmon, void* lpmi) {
  (void)hmon;
  if (!lpmi) return 0;
  u32 cb = *(u32*)lpmi;
  if (cb < 40) return 0;
  int w = 1920, h = 1080;
  trt_display_size(&w, &h);
  if (w <= 0) w = 1920;
  if (h <= 0) h = 1080;
  struct mi_base {
    u32 cbSize;
    rect_i32 rcMonitor;
    rect_i32 rcWork;
    u32 dwFlags;
  };
  mi_base* m = (mi_base*)lpmi;
  m->rcMonitor = {0, 0, w, h};
  m->rcWork = {0, 0, w, h};
  m->dwFlags = 1;
  if (cb >= 72) {
    char* dev = (char*)lpmi + 40;
    memset(dev, 0, 32);
    snprintf(dev, 32, "\\\\.\\DISPLAY1");
  }
  return 1;
}

using mon_enum_proc_t = int(TRT_GUEST_ABI*)(void* hmon, void* hdc, void* lprc, i64 data);
int TRT_STDCALL trt_EnumDisplayMonitors(void* hdc, const void* lprcClip,
                                        void* lpfnEnum, i64 dwData) {
  (void)lprcClip;
  if (!lpfnEnum) return 0;
  int w = 1920, h = 1080;
  trt_display_size(&w, &h);
  rect_i32 r = {0, 0, w > 0 ? w : 1920, h > 0 ? h : 1080};
  ((mon_enum_proc_t)lpfnEnum)((void*)(uintptr_t)k_primary_hmonitor, hdc, &r, dwData);
  return 1;
}

// DEVMODEW (220 bytes): dmFields at +72, dmPelsWidth at +172, dmPelsHeight at +176,
// dmBitsPerPel at +168, dmDisplayFrequency at +184.
// DEVMODEA (156 bytes): dmFields at +40, dmBitsPerPel at +104, dmPelsWidth at +108,
// dmPelsHeight at +112, dmDisplayFrequency at +120.
int TRT_STDCALL trt_EnumDisplaySettingsW(const u16* devName, u32 modeNum,
                                         void* devMode) {
  (void)devName;
  if (!devMode) return 0;
  // Return 1 mode (modeNum == 0) + ENUM_CURRENT_SETTINGS (-1) + ENUM_REGISTRY_SETTINGS (-2)
  if (modeNum != 0 && modeNum != (u32)-1 && modeNum != (u32)-2) return 0;
  int w = 1920, h = 1080;
  trt_display_size(&w, &h);
  u8* dm = (u8*)devMode;
  memset(dm, 0, 220);
  *(u16*)(dm + 68) = 220;  // dmSize
  *(u32*)(dm + 72) = 0x00040000u | 0x00080000u | 0x00100000u | 0x00400000u;
  *(u32*)(dm + 168) = 32;  // dmBitsPerPel
  *(u32*)(dm + 172) = (u32)(w > 0 ? w : 1920);  // dmPelsWidth
  *(u32*)(dm + 176) = (u32)(h > 0 ? h : 1080);  // dmPelsHeight
  *(u32*)(dm + 184) = 60;  // dmDisplayFrequency
  return 1;
}

int TRT_STDCALL trt_EnumDisplaySettingsExW(const u16* devName, u32 modeNum,
                                           void* devMode, u32 flags) {
  (void)flags;
  return trt_EnumDisplaySettingsW(devName, modeNum, devMode);
}

int TRT_STDCALL trt_EnumDisplaySettingsA(const char* devName, u32 modeNum,
                                         void* devMode) {
  (void)devName;
  if (!devMode) return 0;
  if (modeNum != 0 && modeNum != (u32)-1 && modeNum != (u32)-2) return 0;
  int w = 1920, h = 1080;
  trt_display_size(&w, &h);
  u8* dm = (u8*)devMode;
  memset(dm, 0, 156);
  *(u16*)(dm + 36) = 156;  // dmSize
  *(u32*)(dm + 40) = 0x00040000u | 0x00080000u | 0x00100000u | 0x00400000u;
  *(u32*)(dm + 104) = 32;
  *(u32*)(dm + 108) = (u32)(w > 0 ? w : 1920);
  *(u32*)(dm + 112) = (u32)(h > 0 ? h : 1080);
  *(u32*)(dm + 120) = 60;
  return 1;
}

int TRT_STDCALL trt_EnumDisplaySettingsExA(const char* devName, u32 modeNum,
                                           void* devMode, u32 flags) {
  (void)flags;
  return trt_EnumDisplaySettingsA(devName, modeNum, devMode);
}

int TRT_STDCALL trt_EnumDisplayDevicesW(const u16* lpDevice, u32 iDevNum,
                                        void* lpDisplayDevice, u32 dwFlags) {
  (void)lpDevice; (void)dwFlags;
  if (!lpDisplayDevice || iDevNum != 0) return 0;
  u8* dd = (u8*)lpDisplayDevice;
  u32 cb = *(u32*)dd;
  if (cb < 840) return 0;
  memset(dd + 4, 0, cb - 4);
  u16* name = (u16*)(dd + 4);
  static const char k_dev[] = "\\\\.\\DISPLAY1";
  for (int i = 0; k_dev[i]; ++i) name[i] = (u16)k_dev[i];
  u16* str = (u16*)(dd + 68);
  static const char k_desc[] = "Xbox Series X|S Display";
  for (int i = 0; k_desc[i]; ++i) str[i] = (u16)k_desc[i];
  *(u32*)(dd + 324) = 0x00000005u;  // DISPLAY_DEVICE_ATTACHED_TO_DESKTOP | PRIMARY_DEVICE
  return 1;
}

int TRT_STDCALL trt_EnumDisplayDevicesA(const char* lpDevice, u32 iDevNum,
                                        void* lpDisplayDevice, u32 dwFlags) {
  (void)lpDevice; (void)dwFlags;
  if (!lpDisplayDevice || iDevNum != 0) return 0;
  u8* dd = (u8*)lpDisplayDevice;
  u32 cb = *(u32*)dd;
  if (cb < 424) return 0;
  memset(dd + 4, 0, cb - 4);
  snprintf((char*)(dd + 4), 32, "\\\\.\\DISPLAY1");
  snprintf((char*)(dd + 36), 128, "Xbox Series X|S Display");
  *(u32*)(dd + 164) = 0x00000005u;
  return 1;
}

i32 TRT_STDCALL trt_ChangeDisplaySettingsW(void* devMode, u32 flags) {
  (void)devMode; (void)flags;
  return 0;  // DISP_CHANGE_SUCCESSFUL
}
i32 TRT_STDCALL trt_ChangeDisplaySettingsA(void* devMode, u32 flags) {
  (void)devMode; (void)flags;
  return 0;
}
i32 TRT_STDCALL trt_ChangeDisplaySettingsExW(const u16* devName, void* devMode,
                                             void* hwnd, u32 flags, void* lParam) {
  (void)devName; (void)devMode; (void)hwnd; (void)flags; (void)lParam;
  return 0;
}
i32 TRT_STDCALL trt_ChangeDisplaySettingsExA(const char* devName, void* devMode,
                                             void* hwnd, u32 flags, void* lParam) {
  (void)devName; (void)devMode; (void)hwnd; (void)flags; (void)lParam;
  return 0;
}

i32 TRT_STDCALL trt_GetDisplayConfigBufferSizes(u32 flags, u32* numPath,
                                                u32* numMode) {
  (void)flags;
  if (numPath) *numPath = 0;
  if (numMode) *numMode = 0;
  return 0;  // ERROR_SUCCESS
}
i32 TRT_STDCALL trt_QueryDisplayConfig(u32 flags, u32* numPath, void* pathArr,
                                       u32* numMode, void* modeArr,
                                       void* topologyId) {
  (void)flags; (void)pathArr; (void)modeArr; (void)topologyId;
  if (numPath) *numPath = 0;
  if (numMode) *numMode = 0;
  return 0;
}
i32 TRT_STDCALL trt_DisplayConfigGetDeviceInfo(void* requestPacket) {
  (void)requestPacket;
  return 50;  // ERROR_NOT_SUPPORTED
}

// ---- Raw Input extensions ----
u32 TRT_STDCALL trt_GetRawInputDeviceList(void* list, u32* numDevices, u32 cbSize) {
  struct rid_list { u64 hDevice; u32 dwType; u32 _pad; };
  static_assert(sizeof(rid_list) == 16, "RAWINPUTDEVICELIST Win64 layout");
  if (!numDevices || cbSize != sizeof(rid_list)) {
    trt_SetLastError(87);  // ERROR_INVALID_PARAMETER
    return (u32)-1;
  }
  // Report the synthetic devices backed by Tiramisu's keyboard and pointer path.
  if (!list) {
    *numDevices = 2;
    return 0;
  }
  if (*numDevices < 2) {
    *numDevices = 2;
    trt_SetLastError(122);  // ERROR_INSUFFICIENT_BUFFER
    return (u32)-1;
  }
  rid_list* out = (rid_list*)list;
  out[0] = {0x00060001ULL, 0 /*RIM_TYPEMOUSE*/, 0};
  out[1] = {0x00060002ULL, 1 /*RIM_TYPEKEYBOARD*/, 0};
  return 2;
}

u32 trt_raw_device_info(void* device, u32 command, void* data,
                        u32* size, bool wide) {
  const auto handle = reinterpret_cast<uintptr_t>(device);
  const bool keyboard = handle == 0x00060002u;
  if (!size || (command != 0x20000007u && command != 0x2000000bu)) {
    trt_SetLastError(87);
    return (u32)-1;
  }
  if (!keyboard && handle != 0x00060001u) {
    trt_SetLastError(6);
    return (u32)-1;
  }
  if (command == 0x20000007u /*RIDI_DEVICENAME*/) {
    const char* name = keyboard ? "Tiramisu Keyboard" : "Tiramisu Mouse";
    const u32 chars = static_cast<u32>(strlen(name) + 1);
    const u32 capacity = data ? *size : 0;
    *size = chars;
    if (!data) return 0;
    if (capacity < chars) {
      trt_SetLastError(122);
      return (u32)-1;
    }
    if (wide) {
      auto* out = static_cast<u16*>(data);
      for (u32 i = 0; i < chars; ++i) out[i] = static_cast<u16>(name[i]);
    } else {
      memcpy(data, name, chars);
    }
    return chars;
  }
  // RID_DEVICE_INFO has a two-DWORD header and a six-DWORD union.
  u32 info[8] = {sizeof(info), keyboard ? 1u : 0u};
  if (keyboard) {
    info[2] = 4;     // enhanced keyboard
    info[5] = 12;    // function keys
    info[6] = 3;     // indicators
    info[7] = 101;   // keys
  } else {
    info[3] = 5;     // buttons
  }
  const u32 capacity = data ? *size : 0;
  *size = sizeof(info);
  if (!data) return 0;
  if (capacity < sizeof(info)) {
    trt_SetLastError(122);
    return (u32)-1;
  }
  memcpy(data, info, sizeof(info));
  return sizeof(info);
}

u32 TRT_STDCALL trt_GetRawInputDeviceInfoW(void* device, u32 command,
                                           void* data, u32* size) {
  return trt_raw_device_info(device, command, data, size, true);
}

u32 TRT_STDCALL trt_GetRawInputDeviceInfoA(void* device, u32 command,
                                           void* data, u32* size) {
  return trt_raw_device_info(device, command, data, size, false);
}
u32 TRT_STDCALL trt_GetRawInputBuffer(void* pData, u32* pcbSize, u32 cbSizeHeader) {
  if (!pcbSize || cbSizeHeader != 24) {
    trt_SetLastError(87);
    return (u32)-1;
  }
  const u32 count = trt_raw_read_buffer(pData, pcbSize);
  static std::atomic<unsigned> raw_buffer_calls{0};
  const unsigned n = raw_buffer_calls.fetch_add(1, std::memory_order_relaxed);
  if (n < 16 || count > 0) {
    char line[144];
    snprintf(line, sizeof(line),
             "input: GetRawInputBuffer call=%u result=%u bytes=%u",
             n + 1, count, *pcbSize);
    trt_core_log(line);
  }
  if (count == (u32)-1) trt_SetLastError(122);
  return count;
}
i64 TRT_STDCALL trt_DefRawInputProc(void** paRawInput, i32 nInput, u32 cbSizeHeader) {
  (void)paRawInput; (void)nInput; (void)cbSizeHeader;
  return 0;
}

// ---- Window geometry & hierarchy ----
int TRT_STDCALL trt_AdjustWindowRectEx(void* lpRect, u32 dwStyle, int bMenu,
                                       u32 dwExStyle) {
  (void)dwStyle; (void)bMenu; (void)dwExStyle;
  return lpRect ? 1 : 0;
}
int TRT_STDCALL trt_AdjustWindowRectExForDpi(void* lpRect, u32 dwStyle,
                                             int bMenu, u32 dwExStyle, u32 dpi) {
  (void)dwStyle; (void)bMenu; (void)dwExStyle; (void)dpi;
  return lpRect ? 1 : 0;
}
int TRT_STDCALL trt_ClientToScreen(void* hwnd, void* lpPoint) {
  (void)hwnd;
  return lpPoint ? 1 : 0;
}
int TRT_STDCALL trt_ScreenToClient(void* hwnd, void* lpPoint) {
  (void)hwnd;
  return lpPoint ? 1 : 0;
}
int TRT_STDCALL trt_MapWindowPoints(void* hWndFrom, void* hWndTo, void* lpPoints,
                                    u32 cPoints) {
  (void)hWndFrom; (void)hWndTo; (void)lpPoints; (void)cPoints;
  return 0;
}
int TRT_STDCALL trt_GetWindowRect(void* hwnd, void* lpRect) {
  if (!lpRect) return 0;
  if (trt_user32_window_rect(hwnd, static_cast<i32*>(lpRect))) return 1;
  int w = 1920, h = 1080;
  trt_display_size(&w, &h);
  rect_i32* r = (rect_i32*)lpRect;
  r->l = 0; r->t = 0;
  r->r = w > 0 ? w : 1920;
  r->b = h > 0 ? h : 1080;
  return 1;
}
int TRT_STDCALL trt_GetWindowPlacement(void* hwnd, void* lpwndpl) {
  (void)hwnd;
  if (!lpwndpl) return 0;
  struct wndpl {
    u32 length; u32 flags; u32 showCmd;
    point_i32 ptMin; point_i32 ptMax; rect_i32 rcNormal;
  };
  wndpl* p = (wndpl*)lpwndpl;
  p->flags = 0;
  p->showCmd = 1;  // SW_SHOWNORMAL
  p->ptMin = {0, 0};
  p->ptMax = {0, 0};
  p->rcNormal = {0, 0, 1920, 1080};
  return 1;
}
int TRT_STDCALL trt_SetWindowPlacement(void* hwnd, const void* lpwndpl) {
  (void)hwnd; (void)lpwndpl;
  return 1;
}
void* TRT_STDCALL trt_GetParent(void* hwnd) { (void)hwnd; return nullptr; }
void* TRT_STDCALL trt_SetParent(void* child, void* newParent) {
  (void)child; (void)newParent;
  return nullptr;
}
void* TRT_STDCALL trt_GetAncestor(void* hwnd, u32 gaFlags) {
  (void)gaFlags;
  return hwnd;
}
void* TRT_STDCALL trt_GetDesktopWindow(void) {
  return (void*)(uintptr_t)0x00010000ULL;
}
void* TRT_STDCALL trt_GetShellWindow(void) {
  return (void*)(uintptr_t)0x00010000ULL;
}
int TRT_STDCALL trt_IsIconic(void* hwnd) { (void)hwnd; return 0; }
int TRT_STDCALL trt_IsZoomed(void* hwnd) { (void)hwnd; return 1; }
int TRT_STDCALL trt_IsWindowUnicode(void* hwnd) { (void)hwnd; return 1; }
int TRT_STDCALL trt_IsWindowEnabled(void* hwnd) { (void)hwnd; return 1; }
int TRT_STDCALL trt_EnableWindow(void* hwnd, int bEnable) {
  (void)hwnd; (void)bEnable;
  return 0;  // previously enabled
}
int TRT_STDCALL trt_IsChild(void* parent, void* hwnd) {
  (void)parent; (void)hwnd;
  return 0;
}
void* TRT_STDCALL trt_WindowFromPoint(u64 pt) {
  (void)pt;
  return trt_GetActiveWindow();
}
void* TRT_STDCALL trt_ChildWindowFromPoint(void* hwnd, u64 pt) {
  (void)pt;
  return hwnd;
}
void* TRT_STDCALL trt_ChildWindowFromPointEx(void* hwnd, u64 pt, u32 flags) {
  (void)pt; (void)flags;
  return hwnd;
}

using wnd_enum_proc_t = int(TRT_GUEST_ABI*)(void* hwnd, i64 lParam);
int TRT_STDCALL trt_EnumWindows(void* lpEnumFunc, i64 lParam) {
  if (!lpEnumFunc) return 0;
  void* active = trt_GetActiveWindow();
  if (active) ((wnd_enum_proc_t)lpEnumFunc)(active, lParam);
  return 1;
}
int TRT_STDCALL trt_EnumChildWindows(void* parent, void* lpEnumFunc, i64 lParam) {
  (void)parent; (void)lpEnumFunc; (void)lParam;
  return 1;
}
int TRT_STDCALL trt_EnumThreadWindows(u32 dwThreadId, void* lpfn, i64 lParam) {
  (void)dwThreadId;
  return trt_EnumWindows(lpfn, lParam);
}
u32 TRT_STDCALL trt_GetWindowThreadProcessId(void* hwnd, u32* lpdwProcessId) {
  (void)hwnd;
  if (lpdwProcessId) *lpdwProcessId = 0x1000u;
  return 0x1004u;
}
void* TRT_STDCALL trt_GetWindow(void* hwnd, u32 uCmd) {
  (void)hwnd; (void)uCmd;
  return nullptr;
}
void* TRT_STDCALL trt_GetTopWindow(void* hwnd) {
  (void)hwnd;
  return trt_GetActiveWindow();
}
int TRT_STDCALL trt_BringWindowToTop(void* hwnd) { (void)hwnd; return 1; }
void* TRT_STDCALL trt_SetActiveWindow(void* hwnd) { return hwnd; }
int TRT_STDCALL trt_AllowSetForegroundWindow(u32 dwProcessId) {
  (void)dwProcessId;
  return 1;
}
int TRT_STDCALL trt_LockSetForegroundWindow(u32 uLockCode) {
  (void)uLockCode;
  return 1;
}
int TRT_STDCALL trt_FlashWindowEx(void* pfwi) { (void)pfwi; return 1; }
int TRT_STDCALL trt_FlashWindow(void* hwnd, int bInvert) {
  (void)hwnd; (void)bInvert;
  return 1;
}
extern "C" int TRT_STDCALL trt_ShowWindow(void* hwnd, int cmd);
int TRT_STDCALL trt_ShowWindowAsync(void* hwnd, int nCmdShow) {
  return trt_ShowWindow(hwnd, nCmdShow);
}
int TRT_STDCALL trt_SetLayeredWindowAttributes(void* hwnd, u32 crKey,
                                               u8 bAlpha, u32 dwFlags) {
  (void)hwnd; (void)crKey; (void)bAlpha; (void)dwFlags;
  return 1;
}
int TRT_STDCALL trt_UpdateLayeredWindow(void* hwnd, void* hdcDst, void* pptDst,
                                        void* psize, void* hdcSrc, void* pptSrc,
                                        u32 crKey, void* pblend, u32 dwFlags) {
  (void)hwnd; (void)hdcDst; (void)pptDst; (void)psize; (void)hdcSrc;
  (void)pptSrc; (void)crKey; (void)pblend; (void)dwFlags;
  return 1;
}
int TRT_STDCALL trt_SetWindowRgn(void* hwnd, void* hRgn, int bRedraw) {
  (void)hwnd; (void)hRgn; (void)bRedraw;
  return 1;
}
int TRT_STDCALL trt_GetWindowRgn(void* hwnd, void* hRgn) {
  (void)hwnd; (void)hRgn;
  return 0;  // ERROR
}

// ---- Clipboard ----
static void* g_clipboard_data = nullptr;
int TRT_STDCALL trt_OpenClipboard(void* hwndNewOwner) {
  (void)hwndNewOwner;
  return 1;
}
int TRT_STDCALL trt_CloseClipboard(void) { return 1; }
int TRT_STDCALL trt_EmptyClipboard(void) {
  g_clipboard_data = nullptr;
  return 1;
}
void* TRT_STDCALL trt_GetClipboardData(u32 uFormat) {
  (void)uFormat;
  return g_clipboard_data;
}
void* TRT_STDCALL trt_SetClipboardData(u32 uFormat, void* hMem) {
  (void)uFormat;
  g_clipboard_data = hMem;
  return hMem;
}
int TRT_STDCALL trt_IsClipboardFormatAvailable(u32 format) {
  (void)format;
  return g_clipboard_data != nullptr ? 1 : 0;
}
u32 TRT_STDCALL trt_RegisterClipboardFormatW(const u16* lpszFormat) {
  (void)lpszFormat;
  return 0xC080u;
}
u32 TRT_STDCALL trt_RegisterClipboardFormatA(const char* lpszFormat) {
  (void)lpszFormat;
  return 0xC080u;
}
int TRT_STDCALL trt_CountClipboardFormats(void) { return 0; }
u32 TRT_STDCALL trt_EnumClipboardFormats(u32 format) {
  (void)format;
  return 0;
}
u32 TRT_STDCALL trt_GetClipboardSequenceNumber(void) { return 1; }
int TRT_STDCALL trt_AddClipboardFormatListener(void* hwnd) {
  (void)hwnd;
  return 1;
}
int TRT_STDCALL trt_RemoveClipboardFormatListener(void* hwnd) {
  (void)hwnd;
  return 1;
}

// ---- Keyboard layout & translation ----
u32 TRT_STDCALL trt_MapVirtualKeyA(u32 code, u32 type) {
  return trt_MapVirtualKeyW(code, type);
}
u32 TRT_STDCALL trt_MapVirtualKeyExW(u32 code, u32 type, void* dwhkl) {
  (void)dwhkl;
  return trt_MapVirtualKeyW(code, type);
}
u32 TRT_STDCALL trt_MapVirtualKeyExA(u32 code, u32 type, void* dwhkl) {
  (void)dwhkl;
  return trt_MapVirtualKeyW(code, type);
}
int TRT_STDCALL trt_ToUnicode(u32 wVirtKey, u32 wScanCode, const u8* lpKeyState,
                              u16* pwszBuff, int cchBuff, u32 wFlags) {
  (void)wScanCode; (void)wFlags;
  if (!pwszBuff || cchBuff <= 0) return 0;
  const u32 ch = trt_vk_to_char_ex(wVirtKey, lpKeyState);
  if (ch == 0) return 0;
  pwszBuff[0] = (u16)ch;
  if (cchBuff > 1) pwszBuff[1] = 0;
  return 1;
}
int TRT_STDCALL trt_ToUnicodeEx(u32 wVirtKey, u32 wScanCode,
                                const u8* lpKeyState, u16* pwszBuff,
                                int cchBuff, u32 wFlags, void* dwhkl) {
  (void)dwhkl;
  return trt_ToUnicode(wVirtKey, wScanCode, lpKeyState, pwszBuff, cchBuff, wFlags);
}
int TRT_STDCALL trt_ToAscii(u32 uVirtKey, u32 uScanCode, const u8* lpKeyState,
                            u16* lpChar, u32 uFlags) {
  (void)uScanCode; (void)uFlags;
  if (!lpChar) return 0;
  const u32 ch = trt_vk_to_char_ex(uVirtKey, lpKeyState);
  if (ch == 0) return 0;
  *lpChar = (u16)(ch & 0xFFu);
  return 1;
}
i16 TRT_STDCALL trt_VkKeyScanW(u16 ch) {
  return trt_char_to_vk_scan(ch);
}
i16 TRT_STDCALL trt_VkKeyScanA(char ch) {
  return trt_VkKeyScanW((u16)(u8)ch);
}
int TRT_STDCALL trt_GetKeyNameTextW(i32 lParam, u16* lpString, int cchSize) {
  (void)lParam;
  if (!lpString || cchSize <= 0) return 0;
  lpString[0] = 'K';
  if (cchSize > 1) lpString[1] = 0;
  return 1;
}
int TRT_STDCALL trt_GetKeyNameTextA(i32 lParam, char* lpString, int cchSize) {
  (void)lParam;
  if (!lpString || cchSize <= 0) return 0;
  return snprintf(lpString, (size_t)cchSize, "K");
}
int TRT_STDCALL trt_GetKeyboardLayoutNameW(u16* pwszKLID) {
  if (!pwszKLID) return 0;
  static const char k_kl[] = "00000409";
  for (int i = 0; i < 9; ++i) pwszKLID[i] = (u16)k_kl[i];
  return 1;
}
int TRT_STDCALL trt_GetKeyboardLayoutNameA(char* pwszKLID) {
  if (!pwszKLID) return 0;
  snprintf(pwszKLID, 9, "00000409");
  return 1;
}
int TRT_STDCALL trt_GetKeyboardLayoutList(int nBuff, void** lpList) {
  if (nBuff == 0 || !lpList) return 1;
  lpList[0] = (void*)(uintptr_t)0x04090409ULL;
  return 1;
}
void* TRT_STDCALL trt_ActivateKeyboardLayout(void* hkl, u32 Flags) {
  (void)Flags;
  return hkl ? hkl : (void*)(uintptr_t)0x04090409ULL;
}

// ---- Cursors & Icons ----
static void* g_current_cursor = (void*)(uintptr_t)0x00020001ULL;
void* TRT_STDCALL trt_SetCursor(void* hCursor) {
  void* prev = g_current_cursor;
  g_current_cursor = hCursor;
  trt_input_set_cursor_shape(hCursor != nullptr);
  return prev;
}
void* TRT_STDCALL trt_GetCursor(void) { return g_current_cursor; }
int TRT_STDCALL trt_GetCursorInfo(void* pci) {
  if (!pci) return 0;
  struct cursor_info {
    u32 cbSize; u32 flags; void* hCursor; point_i32 ptScreenPos;
  };
  cursor_info* c = (cursor_info*)pci;
  c->hCursor = g_current_cursor;
  int x = 0, y = 0;
  c->flags = trt_cursor_snapshot(&x, &y) ? 1u : 0u;
  c->ptScreenPos = {x, y};
  return 1;
}
int TRT_STDCALL trt_DestroyCursor(void* hCursor) { (void)hCursor; return 1; }
void* TRT_STDCALL trt_LoadCursorA(void* inst, const char* name) {
  (void)inst; (void)name;
  return (void*)(uintptr_t)0x00020001ULL;
}
void* TRT_STDCALL trt_LoadCursorFromFileW(const u16* file) {
  (void)file;
  return (void*)(uintptr_t)0x00020001ULL;
}
void* TRT_STDCALL trt_LoadIconA(void* inst, const char* name) {
  (void)inst; (void)name;
  return (void*)(uintptr_t)0x00030001ULL;
}
void* TRT_STDCALL trt_LoadImageW(void* inst, const u16* name, u32 type,
                                 int cx, int cy, u32 flags) {
  (void)inst; (void)name; (void)cx; (void)cy; (void)flags;
  if (type == 2 /*IMAGE_CURSOR*/) return (void*)(uintptr_t)0x00020001ULL;
  if (type == 1 /*IMAGE_ICON*/) return (void*)(uintptr_t)0x00030001ULL;
  return nullptr;
}
void* TRT_STDCALL trt_CreateIconIndirect(void* piconinfo) {
  (void)piconinfo;
  return (void*)(uintptr_t)0x00030002ULL;
}
void* TRT_STDCALL trt_CopyIcon(void* hIcon) {
  return hIcon ? hIcon : (void*)(uintptr_t)0x00030001ULL;
}
int TRT_STDCALL trt_DrawIconEx(void* hdc, int xLeft, int yTop, void* hIcon,
                               int cxWidth, int cyWidth, u32 istepIfAniCur,
                               void* hbrFlickerFreeDraw, u32 diFlags) {
  (void)hdc; (void)xLeft; (void)yTop; (void)hIcon; (void)cxWidth;
  (void)cyWidth; (void)istepIfAniCur; (void)hbrFlickerFreeDraw; (void)diFlags;
  return 1;
}
int TRT_STDCALL trt_DrawIcon(void* hdc, int x, int y, void* hIcon) {
  (void)hdc; (void)x; (void)y; (void)hIcon;
  return 1;
}
int TRT_STDCALL trt_GetClipCursor(void* lpRect) {
  if (!lpRect) return 0;
  i32* rc = (i32*)lpRect;
  if (trt_input_get_clip_rect(&rc[0], &rc[1], &rc[2], &rc[3])) return 1;
  return trt_GetWindowRect(nullptr, lpRect);
}

// ---- Window Properties (GetProp/SetProp/RemoveProp) ----
static void prop_key_w(const u16* lpString, char* out) {
  if (((uintptr_t)lpString >> 16) == 0) {
    snprintf(out, 64, "#%u", (u32)(uintptr_t)lpString);
  } else {
    int i = 0;
    for (; lpString[i] && i < 63; ++i) out[i] = (char)lpString[i];
    out[i] = 0;
  }
}
static void prop_key_a(const char* lpString, char* out) {
  if (((uintptr_t)lpString >> 16) == 0) {
    snprintf(out, 64, "#%u", (u32)(uintptr_t)lpString);
  } else {
    snprintf(out, 64, "%s", lpString);
  }
}
int TRT_STDCALL trt_SetPropA(u64 hwnd, const char* lpString, void* hData) {
  char key[64];
  prop_key_a(lpString, key);
  for (int i = 0; i < 64; ++i) {
    if (g_props[i].used && g_props[i].hwnd == hwnd && strcmp(g_props[i].name, key) == 0) {
      g_props[i].data = hData;
      return 1;
    }
  }
  for (int i = 0; i < 64; ++i) {
    if (!g_props[i].used) {
      g_props[i].used = true;
      g_props[i].hwnd = hwnd;
      memcpy(g_props[i].name, key, 64);
      g_props[i].data = hData;
      return 1;
    }
  }
  return 0;
}
void* TRT_STDCALL trt_GetPropA(u64 hwnd, const char* lpString) {
  char key[64];
  prop_key_a(lpString, key);
  for (int i = 0; i < 64; ++i)
    if (g_props[i].used && g_props[i].hwnd == hwnd && strcmp(g_props[i].name, key) == 0)
      return g_props[i].data;
  return nullptr;
}
void* TRT_STDCALL trt_RemovePropA(u64 hwnd, const char* lpString) {
  char key[64];
  prop_key_a(lpString, key);
  for (int i = 0; i < 64; ++i) {
    if (g_props[i].used && g_props[i].hwnd == hwnd && strcmp(g_props[i].name, key) == 0) {
      g_props[i].used = false;
      return g_props[i].data;
    }
  }
  return nullptr;
}
int TRT_STDCALL trt_SetPropW(u64 hwnd, const u16* lpString, void* hData) {
  char key[64];
  prop_key_w(lpString, key);
  return trt_SetPropA(hwnd, key, hData);
}
void* TRT_STDCALL trt_GetPropW(u64 hwnd, const u16* lpString) {
  char key[64];
  prop_key_w(lpString, key);
  return trt_GetPropA(hwnd, key);
}
void* TRT_STDCALL trt_RemovePropW(u64 hwnd, const u16* lpString) {
  char key[64];
  prop_key_w(lpString, key);
  return trt_RemovePropA(hwnd, key);
}

// ---- Device notifications & mouse tracking ----
void* TRT_STDCALL trt_RegisterDeviceNotificationW(void* hRecipient,
                                                  void* NotificationFilter,
                                                  u32 Flags) {
  (void)hRecipient; (void)NotificationFilter; (void)Flags;
  return (void*)(uintptr_t)0x00070001ULL;
}
void* TRT_STDCALL trt_RegisterDeviceNotificationA(void* hRecipient,
                                                  void* NotificationFilter,
                                                  u32 Flags) {
  (void)hRecipient; (void)NotificationFilter; (void)Flags;
  return (void*)(uintptr_t)0x00070001ULL;
}
int TRT_STDCALL trt_UnregisterDeviceNotification(void* Handle) {
  (void)Handle;
  return 1;
}
int TRT_STDCALL trt_TrackMouseEvent(void* lpEventTrack) {
  (void)lpEventTrack;
  return 1;
}
int TRT_STDCALL trt_GetMouseMovePointsEx(u32 cbSize, void* lppt,
                                         void* lpptBuf, int nBufPoints,
                                         u32 resolution) {
  (void)cbSize; (void)lppt; (void)lpptBuf; (void)nBufPoints; (void)resolution;
  return -1;
}

// ---- Message pump & dispatch extensions ----
using guest_wndproc_t = i64(TRT_GUEST_ABI*)(void* hwnd, u32 msg, u64 wp, i64 lp);
i64 TRT_STDCALL trt_CallWindowProcW(void* lpPrevWndFunc, void* hWnd, u32 Msg,
                                    u64 wParam, i64 lParam) {
  if (!lpPrevWndFunc) return 0;
  return ((guest_wndproc_t)lpPrevWndFunc)(hWnd, Msg, wParam, lParam);
}
i64 TRT_STDCALL trt_CallWindowProcA(void* lpPrevWndFunc, void* hWnd, u32 Msg,
                                    u64 wParam, i64 lParam) {
  return trt_CallWindowProcW(lpPrevWndFunc, hWnd, Msg, wParam, lParam);
}
i64 TRT_STDCALL trt_DefWindowProcA(void* hwnd, u32 msg, u64 wp, i64 lp) {
  return trt_DefWindowProcW(hwnd, msg, wp, lp);
}
int TRT_STDCALL trt_PeekMessageA(void* out, void* hwnd, u32 min, u32 max, u32 remove) {
  return trt_PeekMessageW(out, hwnd, min, max, remove);
}
int TRT_STDCALL trt_GetMessageA(void* out, void* hwnd, u32 min, u32 max) {
  return trt_GetMessageW(out, hwnd, min, max);
}
i64 TRT_STDCALL trt_DispatchMessageA(const void* m) {
  return trt_DispatchMessageW(m);
}
i64 TRT_STDCALL trt_SendMessageA(void* hwnd, u32 msg, u64 wp, i64 lp) {
  return trt_SendMessageW(hwnd, msg, wp, lp);
}
int TRT_STDCALL trt_PostMessageA(void* hwnd, u32 msg, u64 wp, i64 lp) {
  return trt_PostMessageW(hwnd, msg, wp, lp);
}
i64 TRT_STDCALL trt_SendMessageTimeoutW(void* hwnd, u32 msg, u64 wp, i64 lp,
                                        u32 fuFlags, u32 uTimeout, u64* lpdwResult) {
  (void)fuFlags; (void)uTimeout;
  i64 r = trt_SendMessageW(hwnd, msg, wp, lp);
  if (lpdwResult) *lpdwResult = (u64)r;
  return 1;
}
i64 TRT_STDCALL trt_SendMessageTimeoutA(void* hwnd, u32 msg, u64 wp, i64 lp,
                                        u32 fuFlags, u32 uTimeout, u64* lpdwResult) {
  return trt_SendMessageTimeoutW(hwnd, msg, wp, lp, fuFlags, uTimeout, lpdwResult);
}
int TRT_STDCALL trt_SendNotifyMessageW(void* hwnd, u32 msg, u64 wp, i64 lp) {
  return trt_PostMessageW(hwnd, msg, wp, lp);
}
int TRT_STDCALL trt_SendNotifyMessageA(void* hwnd, u32 msg, u64 wp, i64 lp) {
  return trt_PostMessageW(hwnd, msg, wp, lp);
}
int TRT_STDCALL trt_PostThreadMessageW(u32 idThread, u32 msg, u64 wp, i64 lp) {
  (void)idThread;
  return trt_msg_post(msg, wp, lp) ? 1 : 0;
}
int TRT_STDCALL trt_PostThreadMessageA(u32 idThread, u32 msg, u64 wp, i64 lp) {
  (void)idThread;
  return trt_msg_post(msg, wp, lp) ? 1 : 0;
}
u32 TRT_STDCALL trt_RegisterWindowMessageW(const u16* lpString) {
  if (!lpString) return 0;
  u32 h = 0x811C9DC5u;
  for (int i = 0; lpString[i]; ++i) h = (h ^ (u32)lpString[i]) * 0x01000193u;
  return 0xC000u + (h & 0x3FFFu);
}
u32 TRT_STDCALL trt_RegisterWindowMessageA(const char* lpString) {
  if (!lpString) return 0;
  u32 h = 0x811C9DC5u;
  for (int i = 0; lpString[i]; ++i) h = (h ^ (u8)lpString[i]) * 0x01000193u;
  return 0xC000u + (h & 0x3FFFu);
}
i64 TRT_STDCALL trt_GetMessageExtraInfo(void) { return 0; }
i64 TRT_STDCALL trt_SetMessageExtraInfo(i64 lParam) { (void)lParam; return 0; }
u32 TRT_STDCALL trt_GetMessagePos(void) {
  int x = 0, y = 0;
  trt_mouse_pos(&x, &y);
  return ((u32)(u16)y << 16) | (u32)(u16)x;
}
i32 TRT_STDCALL trt_GetMessageTime(void) { return 0; }
u32 TRT_STDCALL trt_GetQueueStatus(u32 flags) {
  (void)flags;
  return trt_msg_pending() ? 0x00FF00FFu : 0u;
}
int TRT_STDCALL trt_WaitMessage(void) { return 1; }
int TRT_STDCALL trt_InSendMessage(void) { return 0; }
int TRT_STDCALL trt_ReplyMessage(i64 lResult) { (void)lResult; return 0; }

u32 TRT_STDCALL trt_MsgWaitForMultipleObjects(u32 count, const u64* handles,
                                              int wait_all, u32 ms, u32 mask) {
  (void)mask;
  return trt_WaitForMultipleObjects(count, handles, wait_all, ms);
}
u32 TRT_STDCALL trt_MsgWaitForMultipleObjectsEx(u32 count, const u64* handles,
                                                u32 ms, u32 mask, u32 flags) {
  return trt_WaitForMultipleObjects(count, handles, (flags & 1) ? 1 : 0, ms);
}

// ---- Caret & Double-click ----
u32 TRT_STDCALL trt_GetCaretBlinkTime(void) { return 530; }
int TRT_STDCALL trt_SetCaretBlinkTime(u32 uMSeconds) { (void)uMSeconds; return 1; }
u32 TRT_STDCALL trt_GetDoubleClickTime(void) { return 500; }
int TRT_STDCALL trt_CreateCaret(void* hWnd, void* hBitmap, int nWidth, int nHeight) {
  (void)hWnd; (void)hBitmap; (void)nWidth; (void)nHeight;
  return 1;
}
int TRT_STDCALL trt_DestroyCaret(void) { return 1; }
int TRT_STDCALL trt_ShowCaret(void* hWnd) { (void)hWnd; return 1; }
int TRT_STDCALL trt_HideCaret(void* hWnd) { (void)hWnd; return 1; }
int TRT_STDCALL trt_SetCaretPos(int X, int Y) { (void)X; (void)Y; return 1; }
int TRT_STDCALL trt_GetCaretPos(void* lpPoint) {
  if (lpPoint) memset(lpPoint, 0, 8);
  return 1;
}

// ---- Menus ----
void* TRT_STDCALL trt_CreatePopupMenu(void) { return (void*)(uintptr_t)0x00080001ULL; }
void* TRT_STDCALL trt_CreateMenu(void) { return (void*)(uintptr_t)0x00080002ULL; }
int TRT_STDCALL trt_DestroyMenu(void* hMenu) { (void)hMenu; return 1; }
int TRT_STDCALL trt_AppendMenuW(void* hMenu, u32 uFlags, u64 uIDNewItem, const u16* lpNewItem) {
  (void)hMenu; (void)uFlags; (void)uIDNewItem; (void)lpNewItem;
  return 1;
}
int TRT_STDCALL trt_AppendMenuA(void* hMenu, u32 uFlags, u64 uIDNewItem, const char* lpNewItem) {
  (void)hMenu; (void)uFlags; (void)uIDNewItem; (void)lpNewItem;
  return 1;
}
int TRT_STDCALL trt_InsertMenuItemW(void* hmenu, u32 item, int fByPosition, const void* lpmi) {
  (void)hmenu; (void)item; (void)fByPosition; (void)lpmi;
  return 1;
}
int TRT_STDCALL trt_InsertMenuItemA(void* hmenu, u32 item, int fByPosition, const void* lpmi) {
  (void)hmenu; (void)item; (void)fByPosition; (void)lpmi;
  return 1;
}
int TRT_STDCALL trt_SetMenuItemInfoW(void* hmenu, u32 item, int fByPos, const void* lpmii) {
  (void)hmenu; (void)item; (void)fByPos; (void)lpmii;
  return 1;
}
int TRT_STDCALL trt_TrackPopupMenu(void* hMenu, u32 uFlags, int x, int y,
                                   int nReserved, void* hWnd, const void* prcRect) {
  (void)hMenu; (void)uFlags; (void)x; (void)y; (void)nReserved; (void)hWnd; (void)prcRect;
  return 0;
}
int TRT_STDCALL trt_TrackPopupMenuEx(void* hMenu, u32 uFlags, int x, int y,
                                     void* hwnd, void* lptpm) {
  (void)hMenu; (void)uFlags; (void)x; (void)y; (void)hwnd; (void)lptpm;
  return 0;
}
void* TRT_STDCALL trt_GetSubMenu(void* hMenu, int nPos) {
  (void)hMenu; (void)nPos;
  return nullptr;
}
void* TRT_STDCALL trt_GetSystemMenu(void* hWnd, int bRevert) {
  (void)hWnd; (void)bRevert;
  return (void*)(uintptr_t)0x00080003ULL;
}
int TRT_STDCALL trt_EnableMenuItem(void* hMenu, u32 uIDEnableItem, u32 uEnable) {
  (void)hMenu; (void)uIDEnableItem; (void)uEnable;
  return 0;
}
u32 TRT_STDCALL trt_CheckMenuItem(void* hMenu, u32 uIDCheckItem, u32 uCheck) {
  (void)hMenu; (void)uIDCheckItem; (void)uCheck;
  return 0;
}
int TRT_STDCALL trt_SetMenu(void* hWnd, void* hMenu) {
  (void)hWnd; (void)hMenu;
  return 1;
}
void* TRT_STDCALL trt_GetMenu(void* hWnd) { (void)hWnd; return nullptr; }
int TRT_STDCALL trt_DrawMenuBar(void* hWnd) { (void)hWnd; return 1; }

// ---- Touch & Pointer stubs ----
int TRT_STDCALL trt_EnableMouseInPointer(int enable) {
  trt_input_enable_mouse_pointer(enable != 0);
  return 1;
}
int TRT_STDCALL trt_IsMouseInPointerEnabled(void) {
  return trt_input_mouse_pointer_enabled();
}
int TRT_STDCALL trt_CloseTouchInputHandle(void* hTouchInput) { (void)hTouchInput; return 1; }
int TRT_STDCALL trt_GetTouchInputInfo(void* hTouchInput, u32 cInputs, void* pInputs, int cbSize) {
  (void)hTouchInput; (void)cInputs; (void)pInputs; (void)cbSize;
  return 0;
}
int TRT_STDCALL trt_RegisterTouchWindow(void* hwnd, u32 ulFlags) {
  (void)hwnd; (void)ulFlags;
  return 1;
}
int TRT_STDCALL trt_UnregisterTouchWindow(void* hwnd) { (void)hwnd; return 1; }
int TRT_STDCALL trt_IsTouchWindow(void* hwnd, u32* pulFlags) {
  (void)hwnd;
  if (pulFlags) *pulFlags = 0;
  return 0;
}
int TRT_STDCALL trt_GetPointerInfo(u32 pointerId, void* pointerInfo) {
  trt_mouse_pointer mouse{};
  if (!pointerInfo || !trt_input_get_mouse_pointer(pointerId, &mouse)) {
    trt_SetLastError(87);
    return 0;
  }
  // POINTER_INFO is a desktop API. Keep its x64 layout independent of
  // whether the host SDK exposes the structure to AppContainer builds.
  struct pointer_info {
    u32 type, id, frame, flags;
    u64 device, window;
    i32 pixel[2], himetric[2], pixel_raw[2], himetric_raw[2];
    u32 time, history;
    i32 wheel;
    u32 key_states;
    u64 performance_count;
    u32 button_change;
  } info{};
  static_assert(sizeof(pointer_info) == 96);
  info.type = 4;  // PT_MOUSE
  info.id = pointerId;
  info.frame = mouse.frame;
  info.flags = mouse.flags;
  info.device = 0x00060001u;
  info.window = mouse.window;
  info.pixel[0] = info.pixel_raw[0] = mouse.x;
  info.pixel[1] = info.pixel_raw[1] = mouse.y;
  info.himetric[0] = info.himetric_raw[0] = mouse.x * 2540 / 96;
  info.himetric[1] = info.himetric_raw[1] = mouse.y * 2540 / 96;
  info.time = mouse.time;
  info.history = 1;
  info.wheel = mouse.wheel;
  info.key_states = mouse.key_states;
  info.button_change = mouse.change;
  memcpy(pointerInfo, &info, sizeof(info));
  return 1;
}
int TRT_STDCALL trt_GetPointerTouchInfo(u32 pointerId, void* touchInfo) {
  (void)pointerId; (void)touchInfo;
  return 0;
}
int TRT_STDCALL trt_GetPointerPenInfo(u32 pointerId, void* penInfo) {
  (void)pointerId; (void)penInfo;
  return 0;
}
int TRT_STDCALL trt_GetPointerType(u32 pointerId, u32* pointerType) {
  trt_mouse_pointer mouse{};
  if (!pointerType || !trt_input_get_mouse_pointer(pointerId, &mouse)) {
    trt_SetLastError(87);
    return 0;
  }
  *pointerType = 4;  // PT_MOUSE
  return 1;
}
int TRT_STDCALL trt_GetPointerCursorId(u32 pointerId, u32* cursorId) {
  (void)pointerId;
  if (cursorId) *cursorId = 0;
  return 1;
}
int TRT_STDCALL trt_RegisterPointerDeviceNotifications(void* window, int notifyRange) {
  (void)window; (void)notifyRange;
  return 1;
}
int TRT_STDCALL trt_GetPointerDevices(u32* deviceCount, void* pointerDevices) {
  (void)pointerDevices;
  if (deviceCount) *deviceCount = 0;
  return 1;
}
int TRT_STDCALL trt_GetPointerInfoHistory(u32 pointerId, u32* entriesCount,
                                          void* pointerInfo) {
  (void)pointerId; (void)pointerInfo;
  if (entriesCount) *entriesCount = 0;
  trt_SetLastError(87);  // ERROR_INVALID_PARAMETER (no live pointer id)
  return 0;
}
int TRT_STDCALL trt_GetPointerTouchInfoHistory(u32 pointerId, u32* entriesCount,
                                               void* touchInfo) {
  (void)pointerId; (void)touchInfo;
  if (entriesCount) *entriesCount = 0;
  trt_SetLastError(87);
  return 0;
}
int TRT_STDCALL trt_GetPointerPenInfoHistory(u32 pointerId, u32* entriesCount,
                                             void* penInfo) {
  (void)pointerId; (void)penInfo;
  if (entriesCount) *entriesCount = 0;
  trt_SetLastError(87);
  return 0;
}
int TRT_STDCALL trt_EnableNonClientDpiScaling(void* hwnd) {
  (void)hwnd;
  return 1;
}
void* TRT_STDCALL trt_RegisterPowerSettingNotification(void* recipient,
                                                       const void* setting_guid,
                                                       u32 flags) {
  (void)recipient; (void)setting_guid; (void)flags;
  return (void*)(uintptr_t)0x50500001ULL;
}
int TRT_STDCALL trt_UnregisterPowerSettingNotification(void* handle) {
  (void)handle;
  return 1;
}

// ---- Desktop & WindowStation ----
int TRT_STDCALL trt_AttachThreadInput(u32 idAttach, u32 idAttachTo, int fAttach) {
  (void)idAttach; (void)idAttachTo; (void)fAttach;
  return 1;
}
void* TRT_STDCALL trt_GetThreadDesktop(u32 dwThreadId) {
  (void)dwThreadId;
  return (void*)(uintptr_t)0x00090001ULL;
}
void* TRT_STDCALL trt_GetProcessWindowStation(void) {
  return (void*)(uintptr_t)0x00090002ULL;
}
int TRT_STDCALL trt_SetProcessWindowStation(void* hWinSta) { (void)hWinSta; return 1; }
int TRT_STDCALL trt_SetThreadDesktop(void* hDesktop) { (void)hDesktop; return 1; }
int TRT_STDCALL trt_GetUserObjectInformationW(void* hObj, int nIndex, void* pvInfo,
                                              u32 nLength, u32* lpnLengthNeeded) {
  (void)hObj;
  // UOI_FLAGS = 1 -> USEROBJECTFLAGS {fInherit=0, fReserved=0, dwFlags=1 (WSF_VISIBLE)}
  if (nIndex == 1 && pvInfo && nLength >= 12) {
    u32* f = (u32*)pvInfo;
    f[0] = 0; f[1] = 0; f[2] = 1;
    if (lpnLengthNeeded) *lpnLengthNeeded = 12;
    return 1;
  }
  if (nIndex == 2 /*UOI_NAME*/ && pvInfo && nLength >= 16) {
    u16* s = (u16*)pvInfo;
    static const char k_def[] = "Default";
    for (int i = 0; i < 8; ++i) s[i] = (u16)k_def[i];
    if (lpnLengthNeeded) *lpnLengthNeeded = 16;
    return 1;
  }
  if (lpnLengthNeeded) *lpnLengthNeeded = 16;
  return 0;
}
int TRT_STDCALL trt_GetUserObjectInformationA(void* hObj, int nIndex, void* pvInfo,
                                              u32 nLength, u32* lpnLengthNeeded) {
  (void)hObj;
  if (nIndex == 1 && pvInfo && nLength >= 12) {
    u32* f = (u32*)pvInfo;
    f[0] = 0; f[1] = 0; f[2] = 1;
    if (lpnLengthNeeded) *lpnLengthNeeded = 12;
    return 1;
  }
  if (nIndex == 2 && pvInfo && nLength >= 8) {
    snprintf((char*)pvInfo, nLength, "Default");
    if (lpnLengthNeeded) *lpnLengthNeeded = 8;
    return 1;
  }
  if (lpnLengthNeeded) *lpnLengthNeeded = 8;
  return 0;
}

// ---- Rect, Paint & DC helpers ----
int TRT_STDCALL trt_InvalidateRect(u64 hwnd, const void* rect, int erase) {
  (void)erase;  // the window is repainted with its own background class
  trt_user32_invalidate((void*)(uintptr_t)hwnd, (const i32*)rect);
  return 1;
}
int TRT_STDCALL trt_ValidateRect(void* hwnd, const void* lpRect) {
  trt_user32_validate(hwnd, (const i32*)lpRect);
  return 1;
}
int TRT_STDCALL trt_InvalidateRgn(void* hwnd, void* hRgn, int bErase) {
  (void)hwnd; (void)hRgn; (void)bErase;
  return 1;
}
int TRT_STDCALL trt_ValidateRgn(void* hwnd, void* hRgn) {
  (void)hwnd; (void)hRgn;
  return 1;
}
int TRT_STDCALL trt_RedrawWindow(void* hwnd, const void* lprcUpdate,
                                 void* hrgnUpdate, u32 flags) {
  (void)hwnd; (void)lprcUpdate; (void)hrgnUpdate; (void)flags;
  return 1;
}
void* TRT_STDCALL trt_BeginPaint(void* hwnd, void* lpPaint) {
  // PAINTSTRUCT on x64 (72 bytes): hdc(8), fErase(4), rcPaint(16), ...
  void* hdc = trt_GetDC(hwnd);
  {
    char line[96];
    snprintf(line, sizeof(line), "user32: BeginPaint(hwnd=0x%llx) -> hdc=%p",
             (unsigned long long)(uintptr_t)hwnd, hdc);
    trt_core_log(line);
  }
  if (lpPaint) {
    memset(lpPaint, 0, 72);
    *(void**)lpPaint = hdc;
    *(i32*)((u8*)lpPaint + 8) = 1;
    rect_i32* rc = (rect_i32*)((u8*)lpPaint + 12);
    // rcPaint is what the window actually owes a repaint to (the paint cycle
    // invalidates the client, InvalidateRect can narrow it), else its client.
    i32 cw = 1920, ch = 1080;
    trt_user32_client_size(hwnd, &cw, &ch);
    i32 upd[4] = {0, 0, cw, ch};
    if (hwnd) trt_user32_update_rect(hwnd, upd);
    rc->l = upd[0];
    rc->t = upd[1];
    rc->r = upd[2];
    rc->b = upd[3];
    if (hwnd) trt_user32_validate(hwnd, nullptr);  // BeginPaint consumes it
  }
  return hdc;
}
int TRT_STDCALL trt_EndPaint(void* hwnd, const void* lpPaint) {
  if (lpPaint) trt_ReleaseDC(hwnd, *(void* const*)lpPaint);
  return 1;
}
void* TRT_STDCALL trt_GetWindowDC(void* hwnd) { return trt_GetDC(hwnd); }
void* TRT_STDCALL trt_GetDCEx(void* hwnd, void* hrgnClip, u32 flags) {
  (void)hrgnClip; (void)flags;
  return trt_GetDC(hwnd);
}
int TRT_STDCALL trt_GetUpdateRect(void* hwnd, void* lpRect, int bErase) {
  (void)bErase;
  i32 rc[4] = {0, 0, 0, 0};
  const int owed = hwnd ? trt_user32_update_rect(hwnd, rc) : 0;
  if (!owed && hwnd) {
    // No explicit invalidation: a window that owes a paint owes its client.
    i32 cw = 0, ch = 0;
    trt_user32_client_size(hwnd, &cw, &ch);
    rc[0] = rc[1] = 0;
    rc[2] = cw;
    rc[3] = ch;
  }
  if (lpRect) memcpy(lpRect, rc, sizeof(rc));
  return owed || hwnd ? 1 : 0;
}
int TRT_STDCALL trt_GetUpdateRgn(void* hwnd, void* hRgn, int bErase) {
  (void)hwnd; (void)hRgn; (void)bErase;
  return 1;  // NULLREGION
}
int TRT_STDCALL trt_LockWindowUpdate(void* hWndLock) { (void)hWndLock; return 1; }
int TRT_STDCALL trt_ScrollWindowEx(void* hWnd, int dx, int dy, const void* prcScroll,
                                   const void* prcClip, void* hrgnUpdate,
                                   void* prcUpdate, u32 flags) {
  // Real scroll: move the window DC's pixels, like GDI does before the app
  // repaints the exposed strip. List panes that blit-and-shift depend on this.
  (void)prcClip;
  (void)hrgnUpdate;
  (void)flags;
  if (!hWnd) return 0;
  void* hdc = trt_GetDC(hWnd);
  if (!hdc) {
    trt_core_log("user32: ScrollWindowEx -> no DC for the window (no scroll)");
    return 0;
  }
  i32 l = 0, t = 0, r = 0, b = 0;
  if (prcScroll) {
    const i32* rc = (const i32*)prcScroll;
    l = rc[0];
    t = rc[1];
    r = rc[2];
    b = rc[3];
  } else {
    i32 cw = 0, ch = 0;
    trt_user32_client_size(hWnd, &cw, &ch);
    r = cw;
    b = ch;
  }
  trt_raster::scroll(hdc, dx, dy, l, t, r, b);
  trt_ReleaseDC(hWnd, hdc);
  if (prcUpdate) {
    // The strip that scrolling exposed is the update rectangle.
    i32* rc = (i32*)prcUpdate;
    if (prcScroll) {
      const i32* s = (const i32*)prcScroll;
      rc[0] = dx > 0 ? s[0] : (dx < 0 ? s[2] + dx : s[0]);
      rc[1] = dy > 0 ? s[1] : (dy < 0 ? s[3] + dy : s[1]);
      rc[2] = dx > 0 ? s[0] + dx : s[2];
      rc[3] = dy > 0 ? s[1] + dy : s[3];
    } else {
      rc[0] = rc[1] = 0;
      rc[2] = r;
      rc[3] = b;
    }
  }
  return 1;
}
// FillRect/FrameRect/InvertRect are the rectangle primitives dialogs and
// controls use to paint themselves; they take the brush as an argument instead
// of a DC selection. They now write the DC's raster surface.
int TRT_STDCALL trt_FillRect(void* hDC, const void* lprc, void* hbr) {
  if (!hDC || !lprc) {
    trt_core_log("user32: FillRect -> 0 (NULL DC or rect)");
    return 0;
  }
  const i32* r = (const i32*)lprc;
  if ((u32)(uintptr_t)hbr == 0x80000005u) return 1;  // NULL_BRUSH: nothing
  trt_raster::fill(hDC, r[0], r[1], r[2], r[3], trt_gdi_brush_color(hbr));
  return 1;
}
int TRT_STDCALL trt_FrameRect(void* hDC, const void* lprc, void* hbr) {
  if (!hDC || !lprc) return 0;
  const i32* r = (const i32*)lprc;
  if ((u32)(uintptr_t)hbr == 0x80000005u) return 1;
  trt_raster::frame(hDC, r[0], r[1], r[2], r[3], trt_gdi_brush_color(hbr), 1);
  return 1;
}
int TRT_STDCALL trt_InvertRect(void* hDC, const void* lprc) {
  if (!hDC || !lprc) return 0;
  const i32* r = (const i32*)lprc;
  trt_raster::blit(hDC, r[0], r[1], r[2] - r[0], r[3] - r[1], hDC, r[0], r[1],
                   0x00550009u /*DSTINVERT*/, 0);
  return 1;
}
int TRT_STDCALL trt_SetRect(void* lprc, int xLeft, int yTop, int xRight, int yBottom) {
  if (!lprc) return 0;
  rect_i32* r = (rect_i32*)lprc;
  r->l = xLeft; r->t = yTop; r->r = xRight; r->b = yBottom;
  return 1;
}
int TRT_STDCALL trt_SetRectEmpty(void* lprc) {
  if (!lprc) return 0;
  memset(lprc, 0, 16);
  return 1;
}
int TRT_STDCALL trt_CopyRect(void* lprcDst, const void* lprcSrc) {
  if (!lprcDst || !lprcSrc) return 0;
  memcpy(lprcDst, lprcSrc, 16);
  return 1;
}
int TRT_STDCALL trt_PtInRect(const void* lprc, u64 pt) {
  if (!lprc) return 0;
  const rect_i32* r = (const rect_i32*)lprc;
  i32 x = (i32)(u32)pt;
  i32 y = (i32)(u32)(pt >> 32);
  return (x >= r->l && x < r->r && y >= r->t && y < r->b) ? 1 : 0;
}
int TRT_STDCALL trt_IntersectRect(void* lprcDst, const void* lprcSrc1, const void* lprcSrc2) {
  if (!lprcDst || !lprcSrc1 || !lprcSrc2) return 0;
  const rect_i32* a = (const rect_i32*)lprcSrc1;
  const rect_i32* b = (const rect_i32*)lprcSrc2;
  rect_i32* o = (rect_i32*)lprcDst;
  o->l = a->l > b->l ? a->l : b->l;
  o->t = a->t > b->t ? a->t : b->t;
  o->r = a->r < b->r ? a->r : b->r;
  o->b = a->b < b->b ? a->b : b->b;
  if (o->l >= o->r || o->t >= o->b) {
    memset(o, 0, 16);
    return 0;
  }
  return 1;
}
int TRT_STDCALL trt_SubtractRect(void* lprcDst, const void* lprcSrc1, const void* lprcSrc2) {
  (void)lprcSrc2;
  return trt_CopyRect(lprcDst, lprcSrc1);
}
int TRT_STDCALL trt_EqualRect(const void* lprc1, const void* lprc2) {
  if (!lprc1 || !lprc2) return 0;
  return memcmp(lprc1, lprc2, 16) == 0 ? 1 : 0;
}
int TRT_STDCALL trt_IsRectEmpty(const void* lprc) {
  if (!lprc) return 1;
  const rect_i32* r = (const rect_i32*)lprc;
  return (r->l >= r->r || r->t >= r->b) ? 1 : 0;
}

int TRT_STDCALL trt_MoveWindow(u64 hwnd, int x, int y, int w, int h,
                               int repaint) {
  // Route the same move/resize through the window registry.
  constexpr u32 no_zorder = 0x0004u, no_activate = 0x0010u, no_redraw = 0x0008u;
  return trt_SetWindowPos(reinterpret_cast<void*>(static_cast<uintptr_t>(hwnd)),
                         nullptr, x, y, w, h,
                         no_zorder | no_activate | (repaint ? 0 : no_redraw));
}

// ---- DPI & System Parameters ----
u32 TRT_STDCALL trt_GetDpiForWindow(void* hwnd) { (void)hwnd; return 96; }
u32 TRT_STDCALL trt_GetDpiForSystem(void) { return 96; }
int TRT_STDCALL trt_SetProcessDPIAware(void) { return 1; }
int TRT_STDCALL trt_SetProcessDpiAwarenessContext(void* value) {
  (void)value;
  return 1;
}
int TRT_STDCALL trt_AreDpiAwarenessContextsEqual(void* dpiContextA, void* dpiContextB) {
  return dpiContextA == dpiContextB ? 1 : 0;
}
void* TRT_STDCALL trt_GetThreadDpiAwarenessContext(void) {
  return (void*)(intptr_t)-4;  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
}
void* TRT_STDCALL trt_GetWindowDpiAwarenessContext(void* hwnd) {
  (void)hwnd;
  return (void*)(intptr_t)-4;
}
int TRT_STDCALL trt_GetSystemMetricsForDpi(int nIndex, u32 dpi) {
  (void)dpi;
  return trt_GetSystemMetrics(nIndex);
}

int TRT_STDCALL trt_SystemParametersInfoW(u32 action, u32 psize, void* param,
                                          u32 flags) {
  (void)psize; (void)flags;
  switch (action) {
    case 0x0030:  // SPI_GETNONCLIENTMETRICS
      if (param) memset(param, 0, 340);
      return 1;
    case 0x0048:  // SPI_GETWORKAREA
      if (param) {
        rect_i32* r = (rect_i32*)param;
        r->l = 0; r->t = 0; r->r = 1920; r->b = 1080;
      }
      return 1;
    case 0x1014:
    case 0x1016:  // SPI_GETSCREENSAVEACTIVE
      if (param) *(int*)param = 0;
      return 1;
    case 0x101D:
    case 0x101E:
      if (param) *(int*)param = 3;
      return 1;
    default:
      return 1;
  }
}
int TRT_STDCALL trt_SystemParametersInfoA(u32 action, u32 psize, void* param,
                                          u32 flags) {
  return trt_SystemParametersInfoW(action, psize, param, flags);
}
int TRT_STDCALL trt_SystemParametersInfoForDpi(u32 uiAction, u32 uiParam,
                                               void* pvParam, u32 fWinIni,
                                               u32 dpi) {
  (void)dpi;
  return trt_SystemParametersInfoW(uiAction, uiParam, pvParam, fWinIni);
}
u32 TRT_STDCALL trt_GetSysColor(int nIndex) {
  (void)nIndex;
  return 0x00202020u;
}
void* TRT_STDCALL trt_GetSysColorBrush(int nIndex) {
  (void)nIndex;
  return (void*)(uintptr_t)0x80000014u;  // BLACK_BRUSH stock handle
}

// ---- Dialogs & MessageBox ----
// DialogBoxParamW used to throw the template away, so an engine's own fatal
// message (Unity's "Failed to initialize Direct3D", UE's "LowLevelFatalError")
// never reached the log. These helpers read the DLGTEMPLATE the guest built and
// name its caption and item text before the API returns IDCANCEL.
static u16 rd16le(const u8* p) { return (u16)(p[0] | ((u16)p[1] << 8)); }
static u32 rd32le(const u8* p) {
  return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static std::string u16_ascii(const u16* s, size_t max_chars) {
  std::string out;
  if (!s) return out;
  for (size_t i = 0; i < max_chars && s[i]; ++i) {
    const u16 c = s[i];
    out.push_back(c < 0x20 ? ' ' : (c < 0x80 ? (char)c : '?'));
  }
  return out;
}
// One variable-length template string: 0x0000 = empty, 0xFFFF = ordinal in the
// next word, otherwise a NUL-terminated UTF-16 string.
static std::string dlg_field(const u8*& p, const u8* end) {
  std::string out;
  if (p + 2 > end) { p = end; return out; }
  const u16 first = rd16le(p);
  if (first == 0) { p += 2; return out; }
  if (first == 0xFFFF) {
    p += (p + 4 <= end) ? 4 : 2;
    return "<ordinal>";
  }
  while (p + 2 <= end) {
    const u16 c = rd16le(p);
    p += 2;
    if (!c) break;
    out.push_back(c < 0x20 ? ' ' : (c < 0x80 ? (char)c : '?'));
  }
  return out;
}
static void dlg_align(const u8* base, const u8*& p, const u8* end) {
  p += (size_t)(4 - ((size_t)(p - base) & 3)) & 3;
  if (p > end) p = end;
}
static void log_dialog_template(const void* tmpl, const char* api) {
  const u8* base = (const u8*)tmpl;
  if (!base) {
    char line[160];
    snprintf(line, sizeof(line), "%s -> -1 (null template; no dialog host)", api);
    LOG(line);
    return;
  }
  const u8* end = base + (64u * 1024u);  // guard against a runaway walk
  const u32 style = rd32le(base);
  const u32 exstyle = rd32le(base + 4);
  const u16 cdit = rd16le(base + 8);
  const i16 x = (i16)rd16le(base + 10), y = (i16)rd16le(base + 12);
  const i16 cx = (i16)rd16le(base + 14), cy = (i16)rd16le(base + 16);
  const u8* p = base + 18;
  const std::string menu = dlg_field(p, end);
  const std::string cls = dlg_field(p, end);
  const std::string title = dlg_field(p, end);
  char head[640];
  snprintf(head, sizeof(head),
           "%s [%s] class=%s menu=%s items=%u style=0x%08x exstyle=0x%08x "
           "size=%dx%d@%d,%d",
           api, title.empty() ? "<no caption>" : title.c_str(),
           cls.empty() ? "default" : cls.c_str(),
           menu.empty() ? "none" : menu.c_str(), (unsigned)cdit, style, exstyle,
           (int)cx, (int)cy, (int)x, (int)y);
  LOG(head);
  for (u16 i = 0; i < cdit && i < 32 && p + 18 <= end; ++i) {
    const u32 istyle = rd32le(p);
    const u16 iid = rd16le(p + 16);
    p += 18;
    const std::string icls = dlg_field(p, end);
    const std::string itext = dlg_field(p, end);
    u16 extra = 0;
    if (p + 2 <= end) { extra = rd16le(p); p += 2; }
    p += (p + extra <= end) ? extra : (size_t)(end - p);
    dlg_align(base, p, end);
    if (itext.empty()) continue;  // a control with no text has nothing to name
    char line[640];
    snprintf(line, sizeof(line), "%s item #%u (%s, style=0x%08x): %s", api,
             (unsigned)iid, icls.empty() ? "default class" : icls.c_str(),
             istyle, itext.c_str());
    LOG(line);
  }
  char tail[192];
  snprintf(tail, sizeof(tail),
           "%s -> -1 (no dialog host: the text above is the game's own "
           "message, not drawn)",
           api);
  LOG(tail);
}

int TRT_STDCALL trt_MessageBoxW(void* parent, const u16* text, const u16* cap,
                                u32 type) {
  (void)parent; (void)type;
  char t[128] = {}, c[64] = {};
  size_t i = 0;
  for (; text && text[i] && i < sizeof(t) - 1; ++i) t[i] = (char)text[i];
  for (i = 0; cap && cap[i] && i < sizeof(c) - 1; ++i) c[i] = (char)cap[i];
  char line[256];
  snprintf(line, sizeof(line), "user32: MessageBoxW [%s] %s -> ID_OK "
           "(no modal UI on console)", c, t);
  LOG(line);
  return 1;
}
int TRT_STDCALL trt_MessageBoxA(void* parent, const char* text, const char* cap,
                                u32 type) {
  char line[256];
  snprintf(line, sizeof(line), "user32: MessageBoxA [%s] %s -> ID_OK "
           "(no modal UI on console)", cap ? cap : "", text ? text : "");
  LOG(line);
  (void)parent; (void)type;
  return 1;
}
int TRT_STDCALL trt_MessageBoxExW(void* hWnd, const u16* lpText,
                                  const u16* lpCaption, u32 uType,
                                  u16 wLanguageId) {
  (void)wLanguageId;
  return trt_MessageBoxW(hWnd, lpText, lpCaption, uType);
}
int TRT_STDCALL trt_MessageBoxExA(void* hWnd, const char* lpText,
                                  const char* lpCaption, u32 uType,
                                  u16 wLanguageId) {
  (void)wLanguageId;
  return trt_MessageBoxA(hWnd, lpText, lpCaption, uType);
}
int TRT_STDCALL trt_MessageBoxIndirectW(const void* lpmbp) {
  if (!lpmbp) return 1;
  const u8* b = (const u8*)lpmbp;
  const u32 cb = rd32le(b);
  // MSGBOXPARAMSW field order. The pointers are guest-width, and cbSize tells
  // which layout the guest built: a PE32 guest's struct is 40 bytes, a PE64
  // guest's is 80.
  const size_t pw = (cb && cb < 56) ? 4 : 8;
  const size_t off = (pw == 8) ? 8 : 4;  // after cbSize + alignment
  auto rd_ptr = [&](size_t o) -> const void* {
    if (pw == 8) {
      u64 v = 0;
      memcpy(&v, b + o, 8);
      return (const void*)(uintptr_t)v;
    }
    u32 v = 0;
    memcpy(&v, b + o, 4);
    return (const void*)(uintptr_t)v;
  };
  const u16* text = (const u16*)rd_ptr(off + 2 * pw);
  const u16* cap = (const u16*)rd_ptr(off + 3 * pw);
  const u32 style = rd32le(b + off + 4 * pw);
  char line[640];
  snprintf(line, sizeof(line),
           "user32: MessageBoxIndirectW [%s] %s (style=0x%08x) -> ID_OK "
           "(no modal UI on console)",
           u16_ascii(cap, 80).c_str(), u16_ascii(text, 400).c_str(), style);
  LOG(line);
  return 1;
}
int TRT_STDCALL trt_MessageBoxIndirectA(const void* lpmbp) {
  if (!lpmbp) return 1;
  const u8* b = (const u8*)lpmbp;
  const u32 cb = rd32le(b);
  const size_t pw = (cb && cb < 56) ? 4 : 8;
  const size_t off = (pw == 8) ? 8 : 4;
  auto rd_ptr = [&](size_t o) -> const void* {
    if (pw == 8) {
      u64 v = 0;
      memcpy(&v, b + o, 8);
      return (const void*)(uintptr_t)v;
    }
    u32 v = 0;
    memcpy(&v, b + o, 4);
    return (const void*)(uintptr_t)v;
  };
  const char* text = (const char*)rd_ptr(off + 2 * pw);
  const char* cap = (const char*)rd_ptr(off + 3 * pw);
  const u32 style = rd32le(b + off + 4 * pw);
  char line[640];
  snprintf(line, sizeof(line),
           "user32: MessageBoxIndirectA [%s] %s (style=0x%08x) -> ID_OK "
           "(no modal UI on console)",
           cap ? cap : "", text ? text : "", style);
  LOG(line);
  return 1;
}

extern "C" int trt_user32_icon_free(void* hicon);
int TRT_STDCALL trt_DestroyIcon(void* icon) {
  if (!icon) return 0;
  return trt_user32_icon_free(icon) ? 1 : 1;  // stock icons remain valid no-op, dynamic icons freed
}

int TRT_STDCALL trt_DrawTextW(void* hdc, u16* text, int count, void* rect,
                              u32 format) {
  if (!hdc || !text || !rect) return 0;
  if (count < 0) {  // -1 = NUL-terminated
    count = 0;
    while (text[count]) ++count;
  }
  return trt_gdi_draw_text(hdc, text, count, rect, format);
}
int TRT_STDCALL trt_DrawTextA(void* hdc, const char* text, int count,
                              void* rect, u32 format) {
  if (!hdc || !text || !rect) return 0;
  if (count < 0) {
    count = 0;
    while (text[count]) ++count;
  }
  if (count > 8192) count = 8192;
  std::vector<u16> wide((size_t)count + 1);
  for (int i = 0; i < count; ++i) wide[(size_t)i] = (u16)(u8)text[i];
  return trt_gdi_draw_text(hdc, wide.data(), count, rect, format);
}
u32 TRT_STDCALL trt_GetDialogBaseUnits(void) {
  return (16u << 16) | 8u;
}
void* TRT_STDCALL trt_DialogBoxIndirectParamW(void* inst, const void* tmpl,
                                              void* parent, void* proc,
                                              i64 init) {
  (void)inst; (void)parent; (void)proc; (void)init;
  log_dialog_template(tmpl, "user32: DialogBoxIndirectParamW");
  return (void*)(uintptr_t)-1;
}
void* TRT_STDCALL trt_DialogBoxParamW(void* inst, const u16* tmpl, void* parent,
                                      void* proc, i64 init) {
  (void)inst; (void)parent; (void)proc; (void)init;
  const uintptr_t v = (uintptr_t)tmpl;
  if (v <= 0xFFFF) {
    // A template from the exe's own resources: the id is all we can name
    // without walking the resource section.
    char line[160];
    snprintf(line, sizeof(line),
             "user32: DialogBoxParamW -> -1 (template resource #%u; no dialog "
             "host)",
             (unsigned)v);
    LOG(line);
  } else {
    log_dialog_template(tmpl, "user32: DialogBoxParamW");
  }
  return (void*)(uintptr_t)-1;
}
int TRT_STDCALL trt_SetDlgItemTextW(void* hDlg, int nIDDlgItem, const u16* lpString) {
  (void)hDlg; (void)nIDDlgItem; (void)lpString;
  return 1;
}
int TRT_STDCALL trt_SetDlgItemTextA(void* hDlg, int nIDDlgItem, const char* lpString) {
  (void)hDlg; (void)nIDDlgItem; (void)lpString;
  return 1;
}
u32 TRT_STDCALL trt_GetDlgItemTextW(void* hDlg, int nIDDlgItem, u16* lpString, int cchMax) {
  (void)hDlg; (void)nIDDlgItem;
  if (lpString && cchMax > 0) lpString[0] = 0;
  return 0;
}
u32 TRT_STDCALL trt_GetDlgItemTextA(void* hDlg, int nIDDlgItem, char* lpString, int cchMax) {
  (void)hDlg; (void)nIDDlgItem;
  if (lpString && cchMax > 0) lpString[0] = 0;
  return 0;
}
int TRT_STDCALL trt_CheckDlgButton(void* hDlg, int nIDButton, u32 uCheck) {
  (void)hDlg; (void)nIDButton; (void)uCheck;
  return 1;
}
u32 TRT_STDCALL trt_IsDlgButtonChecked(void* hDlg, int nIDButton) {
  (void)hDlg; (void)nIDButton;
  return 0;
}
i64 TRT_STDCALL trt_SendDlgItemMessageW(void* hDlg, int nIDDlgItem, u32 Msg,
                                        u64 wParam, i64 lParam) {
  (void)hDlg; (void)nIDDlgItem; (void)Msg; (void)wParam; (void)lParam;
  return 0;
}
int TRT_STDCALL trt_IsDialogMessageW(void* hDlg, void* lpMsg) {
  (void)hDlg; (void)lpMsg;
  return 0;
}
int TRT_STDCALL trt_IsDialogMessageA(void* hDlg, void* lpMsg) {
  (void)hDlg; (void)lpMsg;
  return 0;
}

int TRT_STDCALL trt_ShutdownBlockReasonCreate(u64 hwnd, const u16* reason) {
  (void)hwnd; (void)reason;
  return 1;
}
int TRT_STDCALL trt_ShutdownBlockReasonDestroy(u64 hwnd) {
  (void)hwnd;
  return 1;
}

// ---- Char / String helpers ----
u16* TRT_STDCALL trt_CharUpperW(u16* lpsz) {
  if (((uintptr_t)lpsz >> 16) == 0) {
    u16 c = (u16)(uintptr_t)lpsz;
    if (c >= 'a' && c <= 'z') c = (u16)(c - 'a' + 'A');
    return (u16*)(uintptr_t)c;
  }
  for (u16* p = lpsz; *p; ++p)
    if (*p >= 'a' && *p <= 'z') *p = (u16)(*p - 'a' + 'A');
  return lpsz;
}
char* TRT_STDCALL trt_CharUpperA(char* lpsz) {
  if (((uintptr_t)lpsz >> 16) == 0) {
    u8 c = (u8)(uintptr_t)lpsz;
    if (c >= 'a' && c <= 'z') c = (u8)(c - 'a' + 'A');
    return (char*)(uintptr_t)c;
  }
  for (char* p = lpsz; *p; ++p)
    if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
  return lpsz;
}
u32 TRT_STDCALL trt_CharUpperBuffW(u16* lpsz, u32 cchLength) {
  if (!lpsz) return 0;
  for (u32 i = 0; i < cchLength; ++i)
    if (lpsz[i] >= 'a' && lpsz[i] <= 'z') lpsz[i] = (u16)(lpsz[i] - 'a' + 'A');
  return cchLength;
}
u16* TRT_STDCALL trt_CharLowerW(u16* lpsz) {
  if (((uintptr_t)lpsz >> 16) == 0) {
    u16 c = (u16)(uintptr_t)lpsz;
    if (c >= 'A' && c <= 'Z') c = (u16)(c - 'A' + 'a');
    return (u16*)(uintptr_t)c;
  }
  for (u16* p = lpsz; *p; ++p)
    if (*p >= 'A' && *p <= 'Z') *p = (u16)(*p - 'A' + 'a');
  return lpsz;
}
char* TRT_STDCALL trt_CharLowerA(char* lpsz) {
  if (((uintptr_t)lpsz >> 16) == 0) {
    u8 c = (u8)(uintptr_t)lpsz;
    if (c >= 'A' && c <= 'Z') c = (u8)(c - 'A' + 'a');
    return (char*)(uintptr_t)c;
  }
  for (char* p = lpsz; *p; ++p)
    if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
  return lpsz;
}
u32 TRT_STDCALL trt_CharLowerBuffW(u16* lpsz, u32 cchLength) {
  if (!lpsz) return 0;
  for (u32 i = 0; i < cchLength; ++i)
    if (lpsz[i] >= 'A' && lpsz[i] <= 'Z') lpsz[i] = (u16)(lpsz[i] - 'A' + 'a');
  return cchLength;
}
u16* TRT_STDCALL trt_CharNextW(const u16* lpsz) {
  if (!lpsz) return nullptr;
  return (u16*)(*lpsz ? lpsz + 1 : lpsz);
}
char* TRT_STDCALL trt_CharNextA(const char* lpsz) {
  if (!lpsz) return nullptr;
  return (char*)(*lpsz ? lpsz + 1 : lpsz);
}
u16* TRT_STDCALL trt_CharPrevW(const u16* lpszStart, const u16* lpszCurrent) {
  if (!lpszStart || !lpszCurrent) return nullptr;
  return (u16*)(lpszCurrent > lpszStart ? lpszCurrent - 1 : lpszStart);
}
char* TRT_STDCALL trt_CharPrevA(const char* lpszStart, const char* lpszCurrent) {
  if (!lpszStart || !lpszCurrent) return nullptr;
  return (char*)(lpszCurrent > lpszStart ? lpszCurrent - 1 : lpszStart);
}
int TRT_STDCALL trt_IsCharAlphaW(u16 ch) {
  return ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) ? 1 : 0;
}
int TRT_STDCALL trt_IsCharAlphaNumericW(u16 ch) {
  return (trt_IsCharAlphaW(ch) || (ch >= '0' && ch <= '9')) ? 1 : 0;
}
int TRT_STDCALL trt_wsprintfA(char* out, const char* fmt, ...) {
  if (!out || !fmt) return 0;
  strcpy(out, fmt);
  return (int)strlen(out);
}
int TRT_STDCALL trt_wsprintfW(u16* out, const u16* fmt, ...) {
  if (!out || !fmt) return 0;
  int i = 0;
  for (; fmt[i]; ++i) out[i] = fmt[i];
  out[i] = 0;
  return i;
}
int TRT_STDCALL trt_wvsprintfA(char* out, const char* fmt, void* arglist) {
  (void)arglist;
  if (!out || !fmt) return 0;
  strcpy(out, fmt);
  return (int)strlen(out);
}
int TRT_STDCALL trt_wvsprintfW(u16* out, const u16* fmt, void* arglist) {
  (void)arglist;
  return trt_wsprintfW(out, fmt);
}

// ---- Hooks & HotKeys ----
int TRT_STDCALL trt_RegisterHotKey(void* hWnd, int id, u32 fsModifiers, u32 vk) {
  (void)hWnd; (void)id; (void)fsModifiers; (void)vk;
  return 1;
}
int TRT_STDCALL trt_UnregisterHotKey(void* hWnd, int id) {
  (void)hWnd; (void)id;
  return 1;
}
void* TRT_STDCALL trt_SetWindowsHookExW(int idHook, void* lpfn, void* hmod, u32 dwThreadId) {
  (void)idHook; (void)lpfn; (void)hmod; (void)dwThreadId;
  return (void*)(uintptr_t)0x000A0001ULL;
}
void* TRT_STDCALL trt_SetWindowsHookExA(int idHook, void* lpfn, void* hmod, u32 dwThreadId) {
  (void)idHook; (void)lpfn; (void)hmod; (void)dwThreadId;
  return (void*)(uintptr_t)0x000A0001ULL;
}
int TRT_STDCALL trt_UnhookWindowsHookEx(void* hhk) { (void)hhk; return 1; }
i64 TRT_STDCALL trt_CallNextHookEx(void* hhk, int nCode, u64 wParam, i64 lParam) {
  (void)hhk; (void)nCode; (void)wParam; (void)lParam;
  return 0;
}
void* TRT_STDCALL trt_SetWinEventHook(u32 eventMin, u32 eventMax, void* hmodWinEventProc,
                                      void* pfnWinEventProc, u32 idProcess, u32 idThread,
                                      u32 dwFlags) {
  (void)eventMin; (void)eventMax; (void)hmodWinEventProc; (void)pfnWinEventProc;
  (void)idProcess; (void)idThread; (void)dwFlags;
  return (void*)(uintptr_t)0x000A0002ULL;
}
int TRT_STDCALL trt_UnhookWinEvent(void* hWinEventHook) {
  (void)hWinEventHook;
  return 1;
}
void TRT_STDCALL trt_NotifyWinEvent(u32 event, void* hwnd, i32 idObject, i32 idChild) {
  (void)event; (void)hwnd; (void)idObject; (void)idChild;
}

static const trt_named_fn k_u32_gap_fns[] = {
  {"RegisterClassW", (void*)&trt_RegisterClassW},
  {"RegisterClassA", (void*)&trt_RegisterClassA},
  {"RegisterClassExA", (void*)&trt_RegisterClassExA},
  {"UnregisterClassW", (void*)&trt_UnregisterClassW},
  {"UnregisterClassA", (void*)&trt_UnregisterClassA},
  {"GetClassInfoExW", (void*)&trt_GetClassInfoExW},
  {"GetClassInfoExA", (void*)&trt_GetClassInfoExA},
  {"GetClassInfoW", (void*)&trt_GetClassInfoW},
  {"GetClassInfoA", (void*)&trt_GetClassInfoA},
  {"GetClassNameW", (void*)&trt_GetClassNameW},
  {"GetClassNameA", (void*)&trt_GetClassNameA},
  {"GetClassLongPtrW", (void*)&trt_GetClassLongPtrW},
  {"GetClassLongPtrA", (void*)&trt_GetClassLongPtrA},
  {"SetClassLongPtrW", (void*)&trt_SetClassLongPtrW},
  {"SetClassLongPtrA", (void*)&trt_SetClassLongPtrA},
  {"GetClassLongW", (void*)&trt_GetClassLongW},
  {"GetClassLongA", (void*)&trt_GetClassLongA},
  {"SetClassLongW", (void*)&trt_SetClassLongW},
  {"SetClassLongA", (void*)&trt_SetClassLongA},
  {"CreateWindowExA", (void*)&trt_CreateWindowExA},
  {"SetWindowLongPtrW", (void*)&trt_SetWindowLongPtrW},
  {"GetWindowLongPtrW", (void*)&trt_GetWindowLongPtrW},
  {"SetWindowLongPtrA", (void*)&trt_SetWindowLongPtrA},
  {"GetWindowLongPtrA", (void*)&trt_GetWindowLongPtrA},
  {"SetWindowLongW", (void*)&trt_SetWindowLongW},
  {"GetWindowLongW", (void*)&trt_GetWindowLongW},
  {"SetWindowLongA", (void*)&trt_SetWindowLongA},
  {"GetWindowTextW", (void*)&trt_GetWindowTextW},
  {"GetWindowTextA", (void*)&trt_GetWindowTextA},
  {"GetWindowTextLengthW", (void*)&trt_GetWindowTextLengthW},
  {"GetWindowTextLengthA", (void*)&trt_GetWindowTextLengthA},
  {"SetWindowTextA", (void*)&trt_SetWindowTextA},
  {"MonitorFromWindow", (void*)&trt_MonitorFromWindow},
  {"MonitorFromRect", (void*)&trt_MonitorFromRect},
  {"MonitorFromPoint", (void*)&trt_MonitorFromPoint},
  {"GetMonitorInfoW", (void*)&trt_GetMonitorInfoW},
  {"GetMonitorInfoA", (void*)&trt_GetMonitorInfoA},
  {"EnumDisplayMonitors", (void*)&trt_EnumDisplayMonitors},
  {"EnumDisplaySettingsW", (void*)&trt_EnumDisplaySettingsW},
  {"EnumDisplaySettingsExW", (void*)&trt_EnumDisplaySettingsExW},
  {"EnumDisplaySettingsA", (void*)&trt_EnumDisplaySettingsA},
  {"EnumDisplaySettingsExA", (void*)&trt_EnumDisplaySettingsExA},
  {"EnumDisplayDevicesW", (void*)&trt_EnumDisplayDevicesW},
  {"EnumDisplayDevicesA", (void*)&trt_EnumDisplayDevicesA},
  {"ChangeDisplaySettingsW", (void*)&trt_ChangeDisplaySettingsW},
  {"ChangeDisplaySettingsA", (void*)&trt_ChangeDisplaySettingsA},
  {"ChangeDisplaySettingsExW", (void*)&trt_ChangeDisplaySettingsExW},
  {"ChangeDisplaySettingsExA", (void*)&trt_ChangeDisplaySettingsExA},
  {"GetDisplayConfigBufferSizes", (void*)&trt_GetDisplayConfigBufferSizes},
  {"QueryDisplayConfig", (void*)&trt_QueryDisplayConfig},
  {"DisplayConfigGetDeviceInfo", (void*)&trt_DisplayConfigGetDeviceInfo},
  {"GetRawInputDeviceList", (void*)&trt_GetRawInputDeviceList},
  {"GetRawInputDeviceInfoW", (void*)&trt_GetRawInputDeviceInfoW},
  {"GetRawInputDeviceInfoA", (void*)&trt_GetRawInputDeviceInfoA},
  {"GetRawInputBuffer", (void*)&trt_GetRawInputBuffer},
  {"DefRawInputProc", (void*)&trt_DefRawInputProc},
  {"AdjustWindowRectEx", (void*)&trt_AdjustWindowRectEx},
  {"AdjustWindowRectExForDpi", (void*)&trt_AdjustWindowRectExForDpi},
  {"ClientToScreen", (void*)&trt_ClientToScreen},
  {"ScreenToClient", (void*)&trt_ScreenToClient},
  {"MapWindowPoints", (void*)&trt_MapWindowPoints},
  {"GetWindowRect", (void*)&trt_GetWindowRect},
  {"GetWindowPlacement", (void*)&trt_GetWindowPlacement},
  {"SetWindowPlacement", (void*)&trt_SetWindowPlacement},
  {"GetParent", (void*)&trt_GetParent},
  {"SetParent", (void*)&trt_SetParent},
  {"GetAncestor", (void*)&trt_GetAncestor},
  {"GetDesktopWindow", (void*)&trt_GetDesktopWindow},
  {"GetShellWindow", (void*)&trt_GetShellWindow},
  {"IsIconic", (void*)&trt_IsIconic},
  {"IsZoomed", (void*)&trt_IsZoomed},
  {"IsWindowUnicode", (void*)&trt_IsWindowUnicode},
  {"IsWindowEnabled", (void*)&trt_IsWindowEnabled},
  {"EnableWindow", (void*)&trt_EnableWindow},
  {"IsChild", (void*)&trt_IsChild},
  {"WindowFromPoint", (void*)&trt_WindowFromPoint},
  {"ChildWindowFromPoint", (void*)&trt_ChildWindowFromPoint},
  {"ChildWindowFromPointEx", (void*)&trt_ChildWindowFromPointEx},
  {"EnumWindows", (void*)&trt_EnumWindows},
  {"EnumChildWindows", (void*)&trt_EnumChildWindows},
  {"EnumThreadWindows", (void*)&trt_EnumThreadWindows},
  {"GetWindowThreadProcessId", (void*)&trt_GetWindowThreadProcessId},
  {"GetWindow", (void*)&trt_GetWindow},
  {"GetTopWindow", (void*)&trt_GetTopWindow},
  {"BringWindowToTop", (void*)&trt_BringWindowToTop},
  {"SetActiveWindow", (void*)&trt_SetActiveWindow},
  {"AllowSetForegroundWindow", (void*)&trt_AllowSetForegroundWindow},
  {"LockSetForegroundWindow", (void*)&trt_LockSetForegroundWindow},
  {"FlashWindowEx", (void*)&trt_FlashWindowEx},
  {"FlashWindow", (void*)&trt_FlashWindow},
  {"ShowWindowAsync", (void*)&trt_ShowWindowAsync},
  {"SetLayeredWindowAttributes", (void*)&trt_SetLayeredWindowAttributes},
  {"UpdateLayeredWindow", (void*)&trt_UpdateLayeredWindow},
  {"SetWindowRgn", (void*)&trt_SetWindowRgn},
  {"GetWindowRgn", (void*)&trt_GetWindowRgn},
  {"OpenClipboard", (void*)&trt_OpenClipboard},
  {"CloseClipboard", (void*)&trt_CloseClipboard},
  {"EmptyClipboard", (void*)&trt_EmptyClipboard},
  {"GetClipboardData", (void*)&trt_GetClipboardData},
  {"SetClipboardData", (void*)&trt_SetClipboardData},
  {"IsClipboardFormatAvailable", (void*)&trt_IsClipboardFormatAvailable},
  {"RegisterClipboardFormatW", (void*)&trt_RegisterClipboardFormatW},
  {"RegisterClipboardFormatA", (void*)&trt_RegisterClipboardFormatA},
  {"CountClipboardFormats", (void*)&trt_CountClipboardFormats},
  {"EnumClipboardFormats", (void*)&trt_EnumClipboardFormats},
  {"GetClipboardSequenceNumber", (void*)&trt_GetClipboardSequenceNumber},
  {"AddClipboardFormatListener", (void*)&trt_AddClipboardFormatListener},
  {"RemoveClipboardFormatListener", (void*)&trt_RemoveClipboardFormatListener},
  {"MapVirtualKeyA", (void*)&trt_MapVirtualKeyA},
  {"MapVirtualKeyExW", (void*)&trt_MapVirtualKeyExW},
  {"MapVirtualKeyExA", (void*)&trt_MapVirtualKeyExA},
  {"ToUnicode", (void*)&trt_ToUnicode},
  {"ToUnicodeEx", (void*)&trt_ToUnicodeEx},
  {"ToAscii", (void*)&trt_ToAscii},
  {"VkKeyScanW", (void*)&trt_VkKeyScanW},
  {"VkKeyScanA", (void*)&trt_VkKeyScanA},
  {"GetKeyNameTextW", (void*)&trt_GetKeyNameTextW},
  {"GetKeyNameTextA", (void*)&trt_GetKeyNameTextA},
  {"GetKeyboardLayoutNameW", (void*)&trt_GetKeyboardLayoutNameW},
  {"GetKeyboardLayoutNameA", (void*)&trt_GetKeyboardLayoutNameA},
  {"GetKeyboardLayoutList", (void*)&trt_GetKeyboardLayoutList},
  {"ActivateKeyboardLayout", (void*)&trt_ActivateKeyboardLayout},
  {"SetCursor", (void*)&trt_SetCursor},
  {"GetCursor", (void*)&trt_GetCursor},
  {"GetCursorInfo", (void*)&trt_GetCursorInfo},
  {"DestroyCursor", (void*)&trt_DestroyCursor},
  {"LoadCursorA", (void*)&trt_LoadCursorA},
  {"LoadCursorFromFileW", (void*)&trt_LoadCursorFromFileW},
  {"LoadIconA", (void*)&trt_LoadIconA},
  {"LoadImageW", (void*)&trt_LoadImageW},
  {"CreateIconIndirect", (void*)&trt_CreateIconIndirect},
  {"CopyIcon", (void*)&trt_CopyIcon},
  {"DrawIconEx", (void*)&trt_DrawIconEx},
  {"DrawIcon", (void*)&trt_DrawIcon},
  {"GetClipCursor", (void*)&trt_GetClipCursor},
  {"SetPropW", (void*)&trt_SetPropW},
  {"GetPropW", (void*)&trt_GetPropW},
  {"RemovePropW", (void*)&trt_RemovePropW},
  {"SetPropA", (void*)&trt_SetPropA},
  {"GetPropA", (void*)&trt_GetPropA},
  {"RemovePropA", (void*)&trt_RemovePropA},
  {"RegisterDeviceNotificationW", (void*)&trt_RegisterDeviceNotificationW},
  {"RegisterDeviceNotificationA", (void*)&trt_RegisterDeviceNotificationA},
  {"UnregisterDeviceNotification", (void*)&trt_UnregisterDeviceNotification},
  {"TrackMouseEvent", (void*)&trt_TrackMouseEvent},
  {"GetMouseMovePointsEx", (void*)&trt_GetMouseMovePointsEx},
  {"CallWindowProcW", (void*)&trt_CallWindowProcW},
  {"CallWindowProcA", (void*)&trt_CallWindowProcA},
  {"DefWindowProcA", (void*)&trt_DefWindowProcA},
  {"PeekMessageA", (void*)&trt_PeekMessageA},
  {"GetMessageA", (void*)&trt_GetMessageA},
  {"DispatchMessageA", (void*)&trt_DispatchMessageA},
  {"SendMessageA", (void*)&trt_SendMessageA},
  {"PostMessageA", (void*)&trt_PostMessageA},
  {"SendMessageTimeoutW", (void*)&trt_SendMessageTimeoutW},
  {"SendMessageTimeoutA", (void*)&trt_SendMessageTimeoutA},
  {"SendNotifyMessageW", (void*)&trt_SendNotifyMessageW},
  {"SendNotifyMessageA", (void*)&trt_SendNotifyMessageA},
  {"PostThreadMessageW", (void*)&trt_PostThreadMessageW},
  {"PostThreadMessageA", (void*)&trt_PostThreadMessageA},
  {"RegisterWindowMessageW", (void*)&trt_RegisterWindowMessageW},
  {"RegisterWindowMessageA", (void*)&trt_RegisterWindowMessageA},
  {"GetMessageExtraInfo", (void*)&trt_GetMessageExtraInfo},
  {"SetMessageExtraInfo", (void*)&trt_SetMessageExtraInfo},
  {"GetMessagePos", (void*)&trt_GetMessagePos},
  {"GetMessageTime", (void*)&trt_GetMessageTime},
  {"GetQueueStatus", (void*)&trt_GetQueueStatus},
  {"WaitMessage", (void*)&trt_WaitMessage},
  {"InSendMessage", (void*)&trt_InSendMessage},
  {"ReplyMessage", (void*)&trt_ReplyMessage},
  {"MsgWaitForMultipleObjects", (void*)&trt_MsgWaitForMultipleObjects},
  {"MsgWaitForMultipleObjectsEx", (void*)&trt_MsgWaitForMultipleObjectsEx},
  {"GetCaretBlinkTime", (void*)&trt_GetCaretBlinkTime},
  {"SetCaretBlinkTime", (void*)&trt_SetCaretBlinkTime},
  {"GetDoubleClickTime", (void*)&trt_GetDoubleClickTime},
  {"CreateCaret", (void*)&trt_CreateCaret},
  {"DestroyCaret", (void*)&trt_DestroyCaret},
  {"ShowCaret", (void*)&trt_ShowCaret},
  {"HideCaret", (void*)&trt_HideCaret},
  {"SetCaretPos", (void*)&trt_SetCaretPos},
  {"GetCaretPos", (void*)&trt_GetCaretPos},
  {"CreatePopupMenu", (void*)&trt_CreatePopupMenu},
  {"CreateMenu", (void*)&trt_CreateMenu},
  {"DestroyMenu", (void*)&trt_DestroyMenu},
  {"AppendMenuW", (void*)&trt_AppendMenuW},
  {"AppendMenuA", (void*)&trt_AppendMenuA},
  {"InsertMenuItemW", (void*)&trt_InsertMenuItemW},
  {"InsertMenuItemA", (void*)&trt_InsertMenuItemA},
  {"SetMenuItemInfoW", (void*)&trt_SetMenuItemInfoW},
  {"TrackPopupMenu", (void*)&trt_TrackPopupMenu},
  {"TrackPopupMenuEx", (void*)&trt_TrackPopupMenuEx},
  {"GetSubMenu", (void*)&trt_GetSubMenu},
  {"GetSystemMenu", (void*)&trt_GetSystemMenu},
  {"EnableMenuItem", (void*)&trt_EnableMenuItem},
  {"CheckMenuItem", (void*)&trt_CheckMenuItem},
  {"SetMenu", (void*)&trt_SetMenu},
  {"GetMenu", (void*)&trt_GetMenu},
  {"DrawMenuBar", (void*)&trt_DrawMenuBar},
  {"EnableMouseInPointer", (void*)&trt_EnableMouseInPointer},
  {"IsMouseInPointerEnabled", (void*)&trt_IsMouseInPointerEnabled},
  {"CloseTouchInputHandle", (void*)&trt_CloseTouchInputHandle},
  {"GetTouchInputInfo", (void*)&trt_GetTouchInputInfo},
  {"RegisterTouchWindow", (void*)&trt_RegisterTouchWindow},
  {"UnregisterTouchWindow", (void*)&trt_UnregisterTouchWindow},
  {"IsTouchWindow", (void*)&trt_IsTouchWindow},
  {"GetPointerInfo", (void*)&trt_GetPointerInfo},
  {"GetPointerTouchInfo", (void*)&trt_GetPointerTouchInfo},
  {"GetPointerPenInfo", (void*)&trt_GetPointerPenInfo},
  {"GetPointerInfoHistory", (void*)&trt_GetPointerInfoHistory},
  {"GetPointerTouchInfoHistory", (void*)&trt_GetPointerTouchInfoHistory},
  {"GetPointerPenInfoHistory", (void*)&trt_GetPointerPenInfoHistory},
  {"EnableNonClientDpiScaling", (void*)&trt_EnableNonClientDpiScaling},
  {"RegisterPowerSettingNotification", (void*)&trt_RegisterPowerSettingNotification},
  {"UnregisterPowerSettingNotification", (void*)&trt_UnregisterPowerSettingNotification},
  {"GetPointerType", (void*)&trt_GetPointerType},
  {"GetPointerCursorId", (void*)&trt_GetPointerCursorId},
  {"RegisterPointerDeviceNotifications", (void*)&trt_RegisterPointerDeviceNotifications},
  {"GetPointerDevices", (void*)&trt_GetPointerDevices},
  {"AttachThreadInput", (void*)&trt_AttachThreadInput},
  {"GetThreadDesktop", (void*)&trt_GetThreadDesktop},
  {"GetProcessWindowStation", (void*)&trt_GetProcessWindowStation},
  {"SetProcessWindowStation", (void*)&trt_SetProcessWindowStation},
  {"SetThreadDesktop", (void*)&trt_SetThreadDesktop},
  {"GetUserObjectInformationW", (void*)&trt_GetUserObjectInformationW},
  {"GetUserObjectInformationA", (void*)&trt_GetUserObjectInformationA},
  {"InvalidateRect", (void*)&trt_InvalidateRect},
  {"ValidateRect", (void*)&trt_ValidateRect},
  {"InvalidateRgn", (void*)&trt_InvalidateRgn},
  {"ValidateRgn", (void*)&trt_ValidateRgn},
  {"RedrawWindow", (void*)&trt_RedrawWindow},
  {"BeginPaint", (void*)&trt_BeginPaint},
  {"EndPaint", (void*)&trt_EndPaint},
  {"GetWindowDC", (void*)&trt_GetWindowDC},
  {"GetDCEx", (void*)&trt_GetDCEx},
  {"GetUpdateRect", (void*)&trt_GetUpdateRect},
  {"GetUpdateRgn", (void*)&trt_GetUpdateRgn},
  {"LockWindowUpdate", (void*)&trt_LockWindowUpdate},
  {"ScrollWindowEx", (void*)&trt_ScrollWindowEx},
  {"FillRect", (void*)&trt_FillRect},
  {"FrameRect", (void*)&trt_FrameRect},
  {"InvertRect", (void*)&trt_InvertRect},
  {"SetRect", (void*)&trt_SetRect},
  {"SetRectEmpty", (void*)&trt_SetRectEmpty},
  {"CopyRect", (void*)&trt_CopyRect},
  {"PtInRect", (void*)&trt_PtInRect},
  {"IntersectRect", (void*)&trt_IntersectRect},
  {"SubtractRect", (void*)&trt_SubtractRect},
  {"EqualRect", (void*)&trt_EqualRect},
  {"IsRectEmpty", (void*)&trt_IsRectEmpty},
  {"MoveWindow", (void*)&trt_MoveWindow},
  {"GetDpiForWindow", (void*)&trt_GetDpiForWindow},
  {"GetDpiForSystem", (void*)&trt_GetDpiForSystem},
  {"SetProcessDPIAware", (void*)&trt_SetProcessDPIAware},
  {"SetProcessDpiAwarenessContext", (void*)&trt_SetProcessDpiAwarenessContext},
  {"AreDpiAwarenessContextsEqual", (void*)&trt_AreDpiAwarenessContextsEqual},
  {"GetThreadDpiAwarenessContext", (void*)&trt_GetThreadDpiAwarenessContext},
  {"GetWindowDpiAwarenessContext", (void*)&trt_GetWindowDpiAwarenessContext},
  {"GetSystemMetricsForDpi", (void*)&trt_GetSystemMetricsForDpi},
  {"SystemParametersInfoW", (void*)&trt_SystemParametersInfoW},
  {"SystemParametersInfoA", (void*)&trt_SystemParametersInfoA},
  {"SystemParametersInfoForDpi", (void*)&trt_SystemParametersInfoForDpi},
  {"GetSysColor", (void*)&trt_GetSysColor},
  {"GetSysColorBrush", (void*)&trt_GetSysColorBrush},
  {"MessageBoxW", (void*)&trt_MessageBoxW},
  {"MessageBoxA", (void*)&trt_MessageBoxA},
  {"MessageBoxExW", (void*)&trt_MessageBoxExW},
  {"MessageBoxExA", (void*)&trt_MessageBoxExA},
  {"MessageBoxIndirectW", (void*)&trt_MessageBoxIndirectW},
  {"MessageBoxIndirectA", (void*)&trt_MessageBoxIndirectA},
  {"DestroyIcon", (void*)&trt_DestroyIcon},
  {"DrawTextW", (void*)&trt_DrawTextW},
  {"DrawTextA", (void*)&trt_DrawTextA},
  {"GetDialogBaseUnits", (void*)&trt_GetDialogBaseUnits},
  {"DialogBoxIndirectParamW", (void*)&trt_DialogBoxIndirectParamW},
  {"DialogBoxParamW", (void*)&trt_DialogBoxParamW},
  {"SetDlgItemTextW", (void*)&trt_SetDlgItemTextW},
  {"SetDlgItemTextA", (void*)&trt_SetDlgItemTextA},
  {"GetDlgItemTextW", (void*)&trt_GetDlgItemTextW},
  {"GetDlgItemTextA", (void*)&trt_GetDlgItemTextA},
  {"CheckDlgButton", (void*)&trt_CheckDlgButton},
  {"IsDlgButtonChecked", (void*)&trt_IsDlgButtonChecked},
  {"SendDlgItemMessageW", (void*)&trt_SendDlgItemMessageW},
  {"IsDialogMessageW", (void*)&trt_IsDialogMessageW},
  {"IsDialogMessageA", (void*)&trt_IsDialogMessageA},
  {"ShutdownBlockReasonCreate", (void*)&trt_ShutdownBlockReasonCreate},
  {"ShutdownBlockReasonDestroy", (void*)&trt_ShutdownBlockReasonDestroy},
  {"CharUpperW", (void*)&trt_CharUpperW},
  {"CharUpperA", (void*)&trt_CharUpperA},
  {"CharUpperBuffW", (void*)&trt_CharUpperBuffW},
  {"CharLowerW", (void*)&trt_CharLowerW},
  {"CharLowerA", (void*)&trt_CharLowerA},
  {"CharLowerBuffW", (void*)&trt_CharLowerBuffW},
  {"CharNextW", (void*)&trt_CharNextW},
  {"CharNextA", (void*)&trt_CharNextA},
  {"CharPrevW", (void*)&trt_CharPrevW},
  {"CharPrevA", (void*)&trt_CharPrevA},
  {"IsCharAlphaW", (void*)&trt_IsCharAlphaW},
  {"IsCharAlphaNumericW", (void*)&trt_IsCharAlphaNumericW},
  {"wsprintfW", (void*)&trt_wsprintfW},
  {"wsprintfA", (void*)&trt_wsprintfA},
  {"wvsprintfW", (void*)&trt_wvsprintfW},
  {"wvsprintfA", (void*)&trt_wvsprintfA},
  {"RegisterHotKey", (void*)&trt_RegisterHotKey},
  {"UnregisterHotKey", (void*)&trt_UnregisterHotKey},
  {"SetWindowsHookExW", (void*)&trt_SetWindowsHookExW},
  {"SetWindowsHookExA", (void*)&trt_SetWindowsHookExA},
  {"UnhookWindowsHookEx", (void*)&trt_UnhookWindowsHookEx},
  {"CallNextHookEx", (void*)&trt_CallNextHookEx},
  {"SetWinEventHook", (void*)&trt_SetWinEventHook},
  {"UnhookWinEvent", (void*)&trt_UnhookWinEvent},
  {"NotifyWinEvent", (void*)&trt_NotifyWinEvent},
};

void trt_u32_gap_register(void) {
  extern void trt_module_register(const char* name, trt_route route,
                                  const trt_named_fn* fns, int count);
  trt_module_register("user32.dll", TRT_ROUTE_BUILTIN, k_u32_gap_fns,
                      (int)(sizeof(k_u32_gap_fns) / sizeof(k_u32_gap_fns[0])));
}

}  // extern "C"
