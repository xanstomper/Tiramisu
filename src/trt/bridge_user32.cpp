// tiramisu/src/trt/bridge_user32.cpp
// Builtin user32 v0.2: input getters (keyboard/mouse real, fed by the host),
// window records, and a WORKING message pump. Semantics follow Wine's
// user32 spec; windows are records, the surface is the host swapchain.
// MSG layout (x64): hwnd(8) message(4+) pad(4) wParam(8) lParam(8) time(4) point(8) = 48
#include "trt.h"
#include "images.h"
#include "thunk.h"

#ifdef _WIN32
#include <windows.h>
#endif
#include "input.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <deque>
#include <mutex>
#include <vector>

namespace user32log {
static void line(const char* s) { trt_core_log(s); }
}
#define log_line(s_) user32log::line(s_)

namespace {

void (*g_pump)() = nullptr;

struct guest_window {
  u64 handle;
  u64 class_atom;
  i64 proc;             // guest WNDPROC
  wchar_t title[64];
};
std::mutex g_win_mu;
std::vector<guest_window> g_wins;
int g_win_count = 0;
u64 g_next_hwnd = 0x00010001;

struct createstruct_w {
  void* lpCreateParams;
  void* hInstance;
  void* hMenu;
  void* hwndParent;
  i32 cy;
  i32 cx;
  i32 y;
  i32 x;
  i32 style;
  u32 _pad1;
  const wchar_t* lpszName;
  const wchar_t* lpszClass;
  u32 dwExStyle;
  u32 _pad2;
};
static_assert(sizeof(createstruct_w) == 80, "Win64 CREATESTRUCTW layout");
std::deque<createstruct_w> g_win_cs;

// Win64 MSG: hwnd u64, message u32, _pad u32, wParam u64, lParam i64,
// time u32, pt {x,y} = 48 bytes
struct guest_msg {
  u64 hwnd;
  u32 message;
  u32 _pad;
  u64 wParam;
  i64 lParam;
  u32 time;
  i32 pt[2];
};
static_assert(sizeof(guest_msg) == 48, "MSG layout drift");

struct guest_timer {
  u64 hwnd;
  u64 id;
  u32 interval_ms;
  void* callback;
  u64 next_fire_ms;
  bool active;
};
std::mutex g_timer_mu;
std::vector<guest_timer> g_timers;
u64 g_next_timer_id = 0x1000;

u64 now_ms64() {
#ifdef _WIN32
  return (u64)GetTickCount64();
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (u64)ts.tv_sec * 1000ULL + (u64)ts.tv_nsec / 1000000ULL;
#endif
}

u32 now_ticks() { return (u32)now_ms64(); }

guest_window* find_win_locked(u64 hwnd) {
  for (guest_window& w : g_wins) {
    if (w.handle == hwnd && hwnd != 0) return &w;
  }
  return nullptr;
}

// extern-C accessors for the gap bridge (bridge_user32_gap.cpp): the window
// record table. The gap bridge mirrors the struct with a static_assert.
extern "C" void* trt_u32_windows(void) {
  if (g_wins.capacity() < 256) g_wins.reserve(256);
  return g_wins.empty() ? nullptr : (void*)g_wins.data();
}
extern "C" int* trt_u32_win_count(void) { return &g_win_count; }
extern "C" i64 TRT_STDCALL trt_SetWindowLongPtrW(u64 hwnd, int idx, i64 value);
extern "C" i64 TRT_STDCALL trt_GetWindowLongPtrW(u64 hwnd, int idx);

i64 dispatch(u64 hwnd, u32 msg, u64 wp, i64 lp) {
  i64 proc = 0;
  {
    std::lock_guard<std::mutex> lk(g_win_mu);
    if (const guest_window* w = find_win_locked(hwnd)) proc = w->proc;
  }
  if (proc) {
    return (i64)trt_call_guest((void*)(uintptr_t)proc, hwnd, msg, wp, (u64)lp);
  }
  return 0;  // DefWindowProcW behavior for record windows
}

} // namespace

extern "C" {

static u32 g_quit_code = 0;
static bool g_quit_posted = false;
static bool g_quit_pending = false;
static void* g_focus_window = nullptr;
static void* g_capture_window = nullptr;

// ---- class/window records ----
u16 TRT_STDCALL trt_RegisterClassExW(const void* wcx) {
  if (!wcx) return 0;
  struct wcxw { u32 cbSize; u32 style; void* proc; i32 cls_x; i32 win_x;
                void* inst; void* icon; void* cursor; void* brush;
                const wchar_t* menu; const wchar_t* name; };
  const wcxw* c = (const wcxw*)wcx;
  if (!c->name) return 0;
  u64 h = 0x84222325;
  for (const wchar_t* p = c->name; *p; ++p) h = (h ^ (*p & 0xFF)) * 0x01000193;
  const u16 atom = (u16)((h & 0x3FFF) | 0xC000);
  std::lock_guard<std::mutex> lk(g_win_mu);
  if (g_wins.capacity() < 256) g_wins.reserve(256);
  for (guest_window& gw : g_wins) {
    if (gw.handle == 0 && gw.class_atom == atom) {
      gw.proc = (i64)(uintptr_t)c->proc;
      return atom;
    }
  }
  guest_window w = {};
  w.class_atom = atom;
  w.proc = (i64)(uintptr_t)c->proc;
  g_wins.push_back(w);
  g_win_count = (int)g_wins.size();
  return atom;
}

static u64 class_atom_from_arg(const wchar_t* cls) {
  const uintptr_t pv = (uintptr_t)cls;
  if (pv != 0 && pv <= 0xFFFFu) return (u64)pv;
  if (!cls) return 0;
  u64 h = 0x84222325;
  for (const wchar_t* p = cls; *p; ++p) h = (h ^ (*p & 0xFF)) * 0x01000193;
  return (u64)((h & 0x3FFF) | 0xC000);
}

void* TRT_STDCALL trt_CreateWindowExW(u32 exstyle, const wchar_t* cls,
                                      const wchar_t* title, u32 style,
                                      int x, int y, int w, int h,
                                      void* parent, void* menu, void* inst,
                                      void* param) {
  u64 hwnd = 0;
  createstruct_w* cs_ptr = nullptr;
  const wchar_t* title_ptr = title;
  bool sync_create = false;
  {
    std::lock_guard<std::mutex> lk(g_win_mu);
    if (g_wins.capacity() < 256) g_wins.reserve(256);
    const u64 target_atom = class_atom_from_arg(cls);
    i64 found_proc = 0;
    u64 found_atom = target_atom;
    bool exact_class = false;
    for (size_t i = g_wins.size(); i > 0; --i) {
      const guest_window& gw = g_wins[i - 1];
      if (gw.handle == 0 && gw.proc &&
          (target_atom == 0 || gw.class_atom == target_atom)) {
        found_proc = gw.proc;
        found_atom = gw.class_atom;
        exact_class = (target_atom != 0 && gw.class_atom == target_atom);
        break;
      }
    }
    if (!found_proc) {
      for (size_t i = g_wins.size(); i > 0; --i) {
        const guest_window& gw = g_wins[i - 1];
        if (gw.handle == 0 && gw.proc) {
          found_proc = gw.proc;
          found_atom = gw.class_atom;
          break;
        }
      }
    }

    guest_window rec = {};
    g_next_hwnd += 0x100;
    hwnd = g_next_hwnd;
    rec.handle = hwnd;
    rec.class_atom = found_atom;
    rec.proc = found_proc;
    if (title) {
      size_t i = 0;
      for (; title[i] && i < 63; ++i) rec.title[i] = title[i];
      rec.title[i] = 0;
    }
    g_wins.push_back(rec);
    g_win_count = (int)g_wins.size();
    title_ptr = g_wins.back().title;

    g_win_cs.emplace_back();
    cs_ptr = &g_win_cs.back();
    memset(cs_ptr, 0, sizeof(*cs_ptr));
    cs_ptr->lpCreateParams = param;
    cs_ptr->hInstance = inst;
    cs_ptr->hMenu = menu;
    cs_ptr->hwndParent = parent;
    cs_ptr->cy = h > 0 ? h : 1080;
    cs_ptr->cx = w > 0 ? w : 1920;
    cs_ptr->y = y;
    cs_ptr->x = x;
    cs_ptr->style = (i32)style;
    cs_ptr->lpszName = title_ptr;
    cs_ptr->lpszClass = cls;
    cs_ptr->dwExStyle = exstyle;
    g_focus_window = (void*)(uintptr_t)hwnd;
    sync_create = (exact_class && found_proc != 0 && param != nullptr);
  }

  trt_SetWindowLongPtrW(hwnd, -16 /*GWL_STYLE*/, (i64)(i32)style);
  trt_SetWindowLongPtrW(hwnd, -20 /*GWL_EXSTYLE*/, (i64)(i32)exstyle);

  char msg[96];
  snprintf(msg, sizeof(msg), "user32: CreateWindowExW -> hwnd=0x%llx",
           (unsigned long long)hwnd);
  log_line(msg);
  if (sync_create) {
    dispatch(hwnd, 0x0081u /*WM_NCCREATE*/, 0, (i64)(uintptr_t)cs_ptr);
    dispatch(hwnd, 0x0001u /*WM_CREATE*/, 0, (i64)(uintptr_t)cs_ptr);
  } else {
    trt_msg_post(0x0001, 0, (i64)(uintptr_t)cs_ptr);  // WM_CREATE
  }
  trt_msg_post(0x0018, 1, 0);                        // WM_SHOWWINDOW
  if (style != 0 && !parent && !(style & 0x40000000u /*WS_CHILD*/)) {
    // The console view already owns input focus. Deliver the activation
    // messages a desktop game receives for its foreground top-level window.
    trt_msg_post(0x001Cu /*WM_ACTIVATEAPP*/, 1, 0);
    trt_msg_post(0x0006u /*WM_ACTIVATE*/, 1 /*WA_ACTIVE*/, 0);
    trt_msg_post(0x0007u /*WM_SETFOCUS*/, 0, 0);
  }
  return (void*)(uintptr_t)hwnd;
}

int TRT_STDCALL trt_DestroyWindow(void* hwnd) {
  const u64 hv = (u64)(uintptr_t)hwnd;
  {
    std::lock_guard<std::mutex> lk(g_win_mu);
    if (g_capture_window == hwnd) g_capture_window = nullptr;
    if (g_focus_window == hwnd) {
      g_focus_window = nullptr;
      for (size_t i = g_wins.size(); i > 0; --i) {
        if (g_wins[i - 1].handle && g_wins[i - 1].handle != hv) {
          g_focus_window = (void*)(uintptr_t)g_wins[i - 1].handle;
          break;
        }
      }
    }
  }
  trt_msg_post(0x0002, 0, (i64)(uintptr_t)hwnd);  // WM_DESTROY
  return 1;
}

int TRT_STDCALL trt_ShowWindow(void*, int) { return 1; }
int TRT_STDCALL trt_UpdateWindow(void*) { return 1; }

i64 TRT_STDCALL trt_DefWindowProcW(void* hwnd, u32 msg, u64 wp, i64 lp) {
  (void)hwnd; (void)wp; (void)lp;
  return (msg == 0x0010u /*WM_CLOSE*/ || msg == 0x0081u /*WM_NCCREATE*/) ? 1 : 0;
}

void* TRT_STDCALL trt_GetActiveWindow(void);
void* TRT_STDCALL trt_GetActiveWindow(void) {
  std::lock_guard<std::mutex> lk(g_win_mu);
  if (g_focus_window && find_win_locked((u64)(uintptr_t)g_focus_window)) {
    return g_focus_window;
  }
  for (size_t i = g_wins.size(); i > 0; --i) {
    if (g_wins[i - 1].handle) return (void*)(uintptr_t)g_wins[i - 1].handle;
  }
  return nullptr;
}

// ---- pump: fill a guest MSG from queued input or due WM_TIMERs ----
static bool fill_timer_msg(guest_msg* m, void* filter_hwnd, u32 min, u32 max,
                           bool remove) {
  if (max != 0 && (0x0113u < min || 0x0113u > max)) return false;
  const u64 now = now_ms64();
  const u64 fhv = (u64)(uintptr_t)filter_hwnd;
  std::lock_guard<std::mutex> lk(g_timer_mu);
  for (guest_timer& t : g_timers) {
    if (!t.active) continue;
    if (fhv && t.hwnd != fhv) continue;
    if (now >= t.next_fire_ms) {
      if (m) {
        m->hwnd = t.hwnd ? t.hwnd : (u64)(uintptr_t)trt_GetActiveWindow();
        m->message = 0x0113u;  // WM_TIMER
        m->_pad = 0;
        m->wParam = t.id;
        m->lParam = (i64)(uintptr_t)t.callback;
        m->time = (u32)now;
        m->pt[0] = 0;
        m->pt[1] = 0;
      }
      if (remove) t.next_fire_ms = now + (t.interval_ms ? t.interval_ms : 1u);
      return true;
    }
  }
  return false;
}

static bool g_pump_logged = false;

static bool msg_in_range(u32 msg, u32 min, u32 max) {
  return (min == 0 && max == 0) || (msg >= min && msg <= max);
}

static bool fill_msg(guest_msg* m, void* filter_hwnd, u32 min, u32 max,
                     bool remove) {
  if (!g_pump_logged) {
    g_pump_logged = true;
    log_line("user32: GetMessageW/PeekMessageW -> message pump entered");
  }
  trt_msg t;
  const bool want_peek = trt_msg_peek_range(&t, min, max, remove);
  if (want_peek) {
    if (remove && t.message == 0x0012u) {
      g_quit_pending = false;
      log_line("user32: message pump dequeued WM_QUIT");
    }
    if (m) {
      int px = 0, py = 0;
      trt_mouse_pos(&px, &py);
      m->hwnd = (t.message == 0x0012u)
                    ? 0
                    : (u64)(uintptr_t)trt_GetActiveWindow();
      m->message = t.message;
      m->_pad = 0;
      m->wParam = t.wparam;
      m->lParam = t.lparam;
      m->time = now_ticks();
      if (remove) trt_input_accept_pointer(&t, m->hwnd, m->time);
      m->pt[0] = px;
      m->pt[1] = py;
    }
    return true;
  }
  if (fill_timer_msg(m, filter_hwnd, min, max, remove)) return true;
  if (g_quit_pending && msg_in_range(0x0012u, min, max)) {
    if (remove) {
      g_quit_pending = false;
      log_line("user32: message pump dequeued WM_QUIT");
    }
    if (m) {
      int px = 0, py = 0;
      trt_mouse_pos(&px, &py);
      m->hwnd = 0;
      m->message = 0x0012u;
      m->_pad = 0;
      m->wParam = (u64)g_quit_code;
      m->lParam = 0;
      m->time = now_ticks();
      m->pt[0] = px;
      m->pt[1] = py;
    }
    return true;
  }
  return false;
}

int TRT_STDCALL trt_GetMessageW(guest_msg* out, void* hwnd, u32 min, u32 max) {
  for (;;) {
    if (g_pump) g_pump();  // keep CoreWindow/XInput alive inside the guest
    if (fill_msg(out, hwnd, min, max, true))
      return out ? ((int)out->message != 0x0012) : 1;  // WM_QUIT->0
#ifdef _WIN32
    ::Sleep(4);
#else
    struct timespec ts{0, 4 * 1000000};
    nanosleep(&ts, nullptr);
#endif
  }
}

void trt_set_pump(void (*fn)(void)) { g_pump = fn; }

void trt_bridge_post_quit(int code) {
  g_quit_code = (u32)code;
  g_quit_posted = true;
  g_quit_pending = true;
}

bool trt_bridge_quit_posted(void) { return g_quit_posted; }

void trt_bridge_set_focus(void* hwnd) { g_focus_window = hwnd; }

int TRT_STDCALL trt_PeekMessageW(guest_msg* out, void* hwnd, u32 min, u32 max,
                                 u32 remove) {
  const bool do_remove = (remove & 1u) != 0;
  if (fill_msg(out, hwnd, min, max, do_remove)) return 1;
  if (g_pump) g_pump();
  return fill_msg(out, hwnd, min, max, do_remove) ? 1 : 0;
}

int TRT_STDCALL trt_PostMessageW(void* hwnd, u32 msg, u64 wp, i64 lp) {
  (void)hwnd;
  return trt_msg_post(msg, wp, lp) ? 1 : 0;
}

int TRT_STDCALL trt_TranslateMessage(const void*) { return 1; }

i64 TRT_STDCALL trt_DispatchMessageW(const guest_msg* m) {
  if (!m) return 0;
  if (m->message == 0x0113u && m->lParam != 0) {
    // TIMERPROC(HWND, UINT, UINT_PTR, DWORD)
    return (i64)trt_call_guest((void*)(uintptr_t)m->lParam, m->hwnd, 0x0113u,
                               m->wParam, (u64)m->time);
  }
  const u64 target = (m->message == 0x0002u && m->lParam != 0)
                         ? (u64)m->lParam
                         : m->hwnd;
  if (!target) return 0;
  if (m->message == 0x0002u || m->message == 0x0010u) {
    char line[110];
    snprintf(line, sizeof(line), "user32: dispatch %s to hwnd=0x%llx",
             m->message == 0x0002u ? "WM_DESTROY" : "WM_CLOSE",
             static_cast<unsigned long long>(target));
    log_line(line);
  }
  const i64 rc = dispatch(target, m->message, m->wParam, m->lParam);
  if (m->message == 0x0002u) {
    std::lock_guard<std::mutex> lk(g_win_mu);
    if (guest_window* w = find_win_locked(target)) w->handle = 0;
  }
  return rc;
}

// ---- input getters ----
i16 TRT_STDCALL trt_GetAsyncKeyState(int vk) { return trt_key_async((u16)vk); }
i16 TRT_STDCALL trt_GetKeyState(int vk) { return trt_key_sync((u16)vk); }

int TRT_STDCALL trt_GetKeyboardState(u8* state) {
  if (!state) return 0;
  memset(state, 0, TRT_MAX_VK);
  for (int i = 0; i < TRT_MAX_VK; ++i) {
    const i16 ks = trt_key_sync((u16)i);
    state[i] = (u8)(((ks < 0) ? 0x80 : 0) | (ks & 0x01));
  }
  return 1;
}

int TRT_STDCALL trt_GetCursorPos(void* pt) {
  struct ptw { i32 x, y; };
  int x = 0, y = 0;
  trt_mouse_pos(&x, &y);
  if (pt) { ((ptw*)pt)->x = x; ((ptw*)pt)->y = y; }
  return 1;
}

int TRT_STDCALL trt_SetCursorPos(int x, int y) {
  trt_input_set_cursor_pos(x, y);
  return 1;
}
int TRT_STDCALL trt_ClipCursor(const void* lpRect) {
  if (!lpRect) {
    trt_input_set_clip_rect(false, 0, 0, 0, 0);
    return 1;
  }
  const i32* rc = (const i32*)lpRect;
  if (rc[2] <= rc[0] || rc[3] <= rc[1]) return 0;
  trt_input_set_clip_rect(true, rc[0], rc[1], rc[2], rc[3]);
  return 1;
}

int TRT_STDCALL trt_GetSystemMetrics(int idx) {
  int w = 0, h = 0;
  trt_display_size(&w, &h);
  switch (idx) {
    case 0: return w;   // SM_CXSCREEN
    case 1: return h;   // SM_CYSCREEN
    case 2: return 1;   // SM_CXVSCROLL
    case 15: return 0;  // SM_SECURE
    default: return 0;
  }
}

void* TRT_STDCALL trt_LoadCursorW(void* inst, const wchar_t* name) {
  (void)inst; (void)name; return (void*)(uintptr_t)0x00020001;
}
void* TRT_STDCALL trt_LoadIconW(void* inst, const wchar_t* name) {
  (void)inst; (void)name; return (void*)(uintptr_t)0x00030001;
}

// ---- Raw Input (the layer most PC engines use for mouse) ----
int TRT_STDCALL trt_RegisterRawInputDevices(const void* devices, u32 count,
                                            u32 size) {
  if (size != 16) return 0;  // sizeof(RAWINPUTDEVICE) on x64
  return trt_raw_register(devices, count) ? 1 : 0;
}

u32 TRT_STDCALL trt_GetRawInputData(void* hraw, u32 cmd, void* out,
                                    u32* out_size, u32 header_size) {
  extern void TRT_STDCALL trt_SetLastError(u32);
  if (!out_size || header_size != 24 ||
      (cmd != 0x10000003u /*RID_INPUT*/ && cmd != 0x10000005u /*RID_HEADER*/)) {
    trt_SetLastError(87);
    return (u32)-1;
  }
  u8 blob[48];
  u32 blob_size = 0;
  if (!trt_raw_copy((u64)(uintptr_t)hraw, blob, &blob_size)) {
    trt_SetLastError(6);
    return (u32)-1;
  }
  if (cmd == 0x10000005u) blob_size = header_size;
  const u32 capacity = out ? *out_size : 0;
  *out_size = blob_size;
  if (!out) return 0;
  if (capacity < blob_size) {
    trt_SetLastError(122);
    return (u32)-1;
  }
  memcpy(out, blob, blob_size);
  return blob_size;
}

namespace {
struct key_map_entry {
  u16 vk;
  u16 scan;
  u8 ext;
  u16 unshifted;  // ToUnicode unshifted (lowercase for A-Z)
  u16 shifted;    // ToUnicode shifted
};

static constexpr key_map_entry k_us_keymap[] = {
    {0x08, 0x0E, 0, '\b', '\b'},  // VK_BACK
    {0x09, 0x0F, 0, '\t', '\t'},  // VK_TAB
    {0x0C, 0x4C, 0, 0, 0},        // VK_CLEAR
    {0x0D, 0x1C, 0, '\r', '\r'},  // VK_RETURN
    {0x10, 0x2A, 0, 0, 0},        // VK_SHIFT
    {0x11, 0x1D, 0, 0, 0},        // VK_CONTROL
    {0x12, 0x38, 0, 0, 0},        // VK_MENU
    {0x13, 0x45, 0, 0, 0},        // VK_PAUSE
    {0x14, 0x3A, 0, 0, 0},        // VK_CAPITAL
    {0x1B, 0x01, 0, 0x1B, 0x1B},  // VK_ESCAPE
    {0x20, 0x39, 0, ' ', ' '},    // VK_SPACE
    {0x21, 0x49, 1, 0, 0},        // VK_PRIOR
    {0x22, 0x51, 1, 0, 0},        // VK_NEXT
    {0x23, 0x4F, 1, 0, 0},        // VK_END
    {0x24, 0x47, 1, 0, 0},        // VK_HOME
    {0x25, 0x4B, 1, 0, 0},        // VK_LEFT
    {0x26, 0x48, 1, 0, 0},        // VK_UP
    {0x27, 0x4D, 1, 0, 0},        // VK_RIGHT
    {0x28, 0x50, 1, 0, 0},        // VK_DOWN
    {0x2C, 0x37, 1, 0, 0},        // VK_SNAPSHOT
    {0x2D, 0x52, 1, 0, 0},        // VK_INSERT
    {0x2E, 0x53, 1, 0, 0},        // VK_DELETE
    {0x30, 0x0B, 0, '0', ')'},
    {0x31, 0x02, 0, '1', '!'},
    {0x32, 0x03, 0, '2', '@'},
    {0x33, 0x04, 0, '3', '#'},
    {0x34, 0x05, 0, '4', '$'},
    {0x35, 0x06, 0, '5', '%'},
    {0x36, 0x07, 0, '6', '^'},
    {0x37, 0x08, 0, '7', '&'},
    {0x38, 0x09, 0, '8', '*'},
    {0x39, 0x0A, 0, '9', '('},
    {'Q',  0x10, 0, 'q', 'Q'}, {'W', 0x11, 0, 'w', 'W'}, {'E', 0x12, 0, 'e', 'E'},
    {'R',  0x13, 0, 'r', 'R'}, {'T', 0x14, 0, 't', 'T'}, {'Y', 0x15, 0, 'y', 'Y'},
    {'U',  0x16, 0, 'u', 'U'}, {'I', 0x17, 0, 'i', 'I'}, {'O', 0x18, 0, 'o', 'O'},
    {'P',  0x19, 0, 'p', 'P'},
    {'A',  0x1E, 0, 'a', 'A'}, {'S', 0x1F, 0, 's', 'S'}, {'D', 0x20, 0, 'd', 'D'},
    {'F',  0x21, 0, 'f', 'F'}, {'G', 0x22, 0, 'g', 'G'}, {'H', 0x23, 0, 'h', 'H'},
    {'J',  0x24, 0, 'j', 'J'}, {'K', 0x25, 0, 'k', 'K'}, {'L', 0x26, 0, 'l', 'L'},
    {'Z',  0x2C, 0, 'z', 'Z'}, {'X', 0x2D, 0, 'x', 'X'}, {'C', 0x2E, 0, 'c', 'C'},
    {'V',  0x2F, 0, 'v', 'V'}, {'B', 0x30, 0, 'b', 'B'}, {'N', 0x31, 0, 'n', 'N'},
    {'M',  0x32, 0, 'm', 'M'},
    {0x5B, 0x5B, 1, 0, 0},  // VK_LWIN
    {0x5C, 0x5C, 1, 0, 0},  // VK_RWIN
    {0x5D, 0x5D, 1, 0, 0},  // VK_APPS
    {0x60, 0x52, 0, '0', '0'}, {0x61, 0x4F, 0, '1', '1'}, {0x62, 0x50, 0, '2', '2'},
    {0x63, 0x51, 0, '3', '3'}, {0x64, 0x4B, 0, '4', '4'}, {0x65, 0x4C, 0, '5', '5'},
    {0x66, 0x4D, 0, '6', '6'}, {0x67, 0x47, 0, '7', '7'}, {0x68, 0x48, 0, '8', '8'},
    {0x69, 0x49, 0, '9', '9'},
    {0x6A, 0x37, 0, '*', '*'},  // VK_MULTIPLY
    {0x6B, 0x4E, 0, '+', '+'},  // VK_ADD
    {0x6D, 0x4A, 0, '-', '-'},  // VK_SUBTRACT
    {0x6E, 0x53, 0, '.', '.'},  // VK_DECIMAL
    {0x6F, 0x35, 1, '/', '/'},  // VK_DIVIDE
    {0x70, 0x3B, 0, 0, 0}, {0x71, 0x3C, 0, 0, 0}, {0x72, 0x3D, 0, 0, 0},
    {0x73, 0x3E, 0, 0, 0}, {0x74, 0x3F, 0, 0, 0}, {0x75, 0x40, 0, 0, 0},
    {0x76, 0x41, 0, 0, 0}, {0x77, 0x42, 0, 0, 0}, {0x78, 0x43, 0, 0, 0},
    {0x79, 0x44, 0, 0, 0}, {0x7A, 0x57, 0, 0, 0}, {0x7B, 0x58, 0, 0, 0},
    {0x90, 0x45, 1, 0, 0},  // VK_NUMLOCK
    {0x91, 0x46, 0, 0, 0},  // VK_SCROLL
    {0xA0, 0x2A, 0, 0, 0},  // VK_LSHIFT
    {0xA1, 0x36, 0, 0, 0},  // VK_RSHIFT
    {0xA2, 0x1D, 0, 0, 0},  // VK_LCONTROL
    {0xA3, 0x1D, 1, 0, 0},  // VK_RCONTROL
    {0xA4, 0x38, 0, 0, 0},  // VK_LMENU
    {0xA5, 0x38, 1, 0, 0},  // VK_RMENU
    {0xBA, 0x27, 0, ';', ':'},   // VK_OEM_1
    {0xBB, 0x0D, 0, '=', '+'},   // VK_OEM_PLUS
    {0xBC, 0x33, 0, ',', '<'},   // VK_OEM_COMMA
    {0xBD, 0x0C, 0, '-', '_'},   // VK_OEM_MINUS
    {0xBE, 0x34, 0, '.', '>'},   // VK_OEM_PERIOD
    {0xBF, 0x35, 0, '/', '?'},   // VK_OEM_2
    {0xC0, 0x29, 0, '`', '~'},   // VK_OEM_3
    {0xDB, 0x1A, 0, '[', '{'},   // VK_OEM_4
    {0xDC, 0x2B, 0, '\\', '|'},  // VK_OEM_5
    {0xDD, 0x1B, 0, ']', '}'},   // VK_OEM_6
    {0xDE, 0x28, 0, '\'', '"'},  // VK_OEM_7
    {0xE2, 0x56, 0, '\\', '|'},  // VK_OEM_102
};

static u32 genericize_lr_vk(u32 vk) {
  if (vk == 0xA0 || vk == 0xA1) return 0x10;
  if (vk == 0xA2 || vk == 0xA3) return 0x11;
  if (vk == 0xA4 || vk == 0xA5) return 0x12;
  return vk;
}
}  // namespace

extern "C" u32 trt_vk_to_char_ex(u32 vk, const u8* ks) {
  const bool shift = ks ? (((ks[0x10] | ks[0xA0] | ks[0xA1]) & 0x80) != 0)
                        : (trt_key_down(0x10) || trt_key_down(0xA0) ||
                           trt_key_down(0xA1));
  const bool ctrl = ks ? (((ks[0x11] | ks[0xA2] | ks[0xA3]) & 0x80) != 0)
                       : (trt_key_down(0x11) || trt_key_down(0xA2) ||
                          trt_key_down(0xA3));
  const bool alt = ks ? (((ks[0x12] | ks[0xA4] | ks[0xA5]) & 0x80) != 0)
                      : (trt_key_down(0x12) || trt_key_down(0xA4) ||
                         trt_key_down(0xA5));
  const bool caps = ks ? ((ks[0x14] & 0x01) != 0) : ((trt_key_sync(0x14) & 1) != 0);
  if (ctrl && !alt) {
    if (vk >= 'A' && vk <= 'Z') return vk - 'A' + 1;
    if (vk == 0xDB) return 0x1B;
    if (vk == 0xDC) return 0x1C;
    if (vk == 0xDD) return 0x1D;
    if (vk == 0x0D) return '\n';
  }
  for (const auto& e : k_us_keymap) {
    if (e.vk != vk || e.unshifted == 0) continue;
    if (vk >= 'A' && vk <= 'Z') {
      return (shift ^ caps) ? e.shifted : e.unshifted;
    }
    return shift ? e.shifted : e.unshifted;
  }
  return 0;
}

extern "C" i16 trt_char_to_vk_scan(u16 ch) {
  for (const auto& e : k_us_keymap) {
    if (e.vk >= 0x60 && e.vk <= 0x6F) continue;  // prefer main keyboard
    if (e.unshifted == ch && ch != 0) return (i16)e.vk;
  }
  for (const auto& e : k_us_keymap) {
    if (e.vk >= 0x60 && e.vk <= 0x6F) continue;
    if (e.shifted == ch && ch != 0) return (i16)(e.vk | 0x0100u);
  }
  return -1;
}

u32 TRT_STDCALL trt_MapVirtualKeyW(u32 code, u32 type) {
  // type 0: VK->scan, 1: scan->VK, 2: VK->char, 3: scan->VK(ex), 4: VK->scan(ex)
  if (type == 0 || type == 4) {
    for (const auto& e : k_us_keymap) {
      if (e.vk == code) {
        if (type == 4 && e.ext) return 0xE000u | e.scan;
        if (type == 4 && code == 0x13) return 0xE11Du;
        return e.scan;
      }
    }
    return trt_key_scan((u16)code);
  }
  if (type == 1 || type == 3) {
    const u8 ext = ((code & 0xFF00u) == 0xE000u) ? 1 : 0;
    const u16 sc = (u16)(code & 0xFFu);
    if (!sc) return 0;
    if (sc == 0x36 && !ext) return (type == 3) ? 0xA1u : 0x10u;
    if (sc == 0x2A && !ext) return (type == 3) ? 0xA0u : 0x10u;
    if (sc == 0x1D) return (type == 3) ? (ext ? 0xA3u : 0xA2u) : 0x11u;
    if (sc == 0x38) return (type == 3) ? (ext ? 0xA5u : 0xA4u) : 0x12u;
    if (ext) {
      for (const auto& e : k_us_keymap) {
        if (e.scan == sc && e.ext) {
          return (type == 1) ? genericize_lr_vk(e.vk) : e.vk;
        }
      }
    }
    for (const auto& e : k_us_keymap) {
      if (e.vk >= 0x60 && e.vk <= 0x6F) continue;  // nav keys beat numpad on Set-1 scan
      if (e.scan == sc && (type == 3 || e.vk != 0xA1)) {
        return (type == 1) ? genericize_lr_vk(e.vk) : e.vk;
      }
    }
    return 0;
  }
  if (type == 2) {
    if (code >= 'A' && code <= 'Z') return code;  // Windows MAPVK_VK_TO_CHAR is uppercase
    for (const auto& e : k_us_keymap) {
      if (e.vk == code) {
        if (code >= 0x60 && code <= 0x69) return 0;
        return e.unshifted;
      }
    }
    return 0;
  }
  return 0;
}

int TRT_STDCALL trt_ShowCursor(int show) {
  return trt_input_show_cursor(show != 0);
}

void* TRT_STDCALL trt_SetCapture(void* hwnd) {
  std::lock_guard<std::mutex> lk(g_win_mu);
  void* prev = g_capture_window;
  g_capture_window = hwnd;
  return prev;
}
int TRT_STDCALL trt_ReleaseCapture(void) {
  std::lock_guard<std::mutex> lk(g_win_mu);
  g_capture_window = nullptr;
  return 1;
}
void* TRT_STDCALL trt_GetCapture(void) {
  std::lock_guard<std::mutex> lk(g_win_mu);
  if (g_capture_window && find_win_locked((u64)(uintptr_t)g_capture_window)) {
    return g_capture_window;
  }
  return nullptr;
}
void* TRT_STDCALL trt_GetForegroundWindow(void) { return trt_GetActiveWindow(); }
u64 TRT_STDCALL trt_GetKeyboardLayout(u32 tid) {
  (void)tid;
  return 0x04090409;  // en-US
}
i64 TRT_STDCALL trt_SendMessageW(void* hwnd, u32 msg, u64 wp, i64 lp) {
  guest_msg m{};
  m.hwnd = (u64)(uintptr_t)hwnd;
  m.message = msg;
  m.wParam = wp;
  m.lParam = lp;
  return trt_DispatchMessageW(&m);
}

// PostQuitMessage(code): the guest asks the loop to end
void TRT_STDCALL trt_PostQuitMessage(i32 code) {
  char line[120];
  snprintf(line, sizeof(line), "user32: PostQuitMessage(%d), caller=%p",
           code, __builtin_return_address(0));
  log_line(line);
  trt_bridge_post_quit(code);
}

// SetFocus(hwnd) / GetFocus(): the guest's input focus
void* TRT_STDCALL trt_SetFocus(void* hwnd) {
  void* prev = g_focus_window;
  g_focus_window = hwnd;
  if (hwnd) {
    trt_msg_post(0x0007 /*WM_SETFOCUS*/, 0, (i64)(uintptr_t)hwnd);
  } else {
    trt_msg_post(0x0008 /*WM_KILLFOCUS*/, 0, 0);
  }
  return prev;
}

void* TRT_STDCALL trt_GetFocus(void) { return g_focus_window; }

// SetWindowPos(hwnd, after, x, y, cx, cy, flags)
int TRT_STDCALL trt_SetWindowPos(void* hwnd, void* after, i32 x, i32 y,
                                  i32 cx, i32 cy, u32 flags) {
  (void)hwnd; (void)after; (void)x; (void)y; (void)cx; (void)cy; (void)flags;
  return 1;
}

// IsWindow / IsWindowVisible: the guest asks whether its window is alive
int TRT_STDCALL trt_IsWindow(void* hwnd) { return hwnd ? 1 : 0; }

int TRT_STDCALL trt_IsWindowVisible(void* hwnd) { return hwnd ? 1 : 0; }

// GetClientRect / GetWindowRect: the guest asks how big it is
int TRT_STDCALL trt_GetClientRect(void* hwnd, void* rect) {
  (void)hwnd;
  if (!rect) return 0;
  auto* r = static_cast<int*>(rect);
  r[0] = 0;
  r[1] = 0;
  trt_display_size(&r[2], &r[3]);
  return 1;
}

u64 TRT_STDCALL trt_SetTimer(void* hwnd, u64 id, u32 ms, void* callback) {
  const u64 hv = (u64)(uintptr_t)hwnd;
  const u32 interval = ms ? ms : 1u;
  const u64 fire = now_ms64() + interval;
  std::lock_guard<std::mutex> lk(g_timer_mu);
  if (hwnd != nullptr || id != 0) {
    for (guest_timer& t : g_timers) {
      if (t.active && t.hwnd == hv && t.id == id) {
        t.interval_ms = interval;
        t.callback = callback;
        t.next_fire_ms = fire;
        return t.id;
      }
    }
  }
  const u64 new_id = (hwnd == nullptr && id == 0) ? (++g_next_timer_id)
                                                  : (id ? id : 1ULL);
  for (guest_timer& t : g_timers) {
    if (!t.active) {
      t = {hv, new_id, interval, callback, fire, true};
      return new_id;
    }
  }
  g_timers.push_back({hv, new_id, interval, callback, fire, true});
  return new_id;
}

int TRT_STDCALL trt_KillTimer(void* hwnd, u64 id) {
  const u64 hv = (u64)(uintptr_t)hwnd;
  std::lock_guard<std::mutex> lk(g_timer_mu);
  for (guest_timer& t : g_timers) {
    if (t.active && t.hwnd == hv && t.id == id) {
      t.active = false;
      return 1;
    }
  }
  return 0;
}

// ---- DC records (uncapped, thread-safe) ----
namespace {
struct user32_dc {
  void* selected;  // currently selected HGDIOBJ (bridge_gdi records here)
  bool used;
};
constexpr u32 USER32_DC_TAG = 0x55430000u;  // 'uC' = user32 DC
std::mutex g_dc_mu;
std::deque<user32_dc> g_dcs;
} // namespace

void* trt_user32_dc_alloc(void) {
  std::lock_guard<std::mutex> lk(g_dc_mu);
  for (size_t i = 0; i < g_dcs.size(); ++i) {
    if (!g_dcs[i].used) {
      g_dcs[i].used = true;
      g_dcs[i].selected = nullptr;
      return (void*)(uintptr_t)(USER32_DC_TAG | (u32)(i + 1));
    }
  }
  g_dcs.push_back({nullptr, true});
  return (void*)(uintptr_t)(USER32_DC_TAG | (u32)g_dcs.size());
}

int trt_user32_dc_release(void* hdc) {
  const u32 h = (u32)(uintptr_t)hdc;
  if ((h & 0xFFFF0000u) != USER32_DC_TAG) return 0;
  const u32 idx = h & 0xFFFFu;
  std::lock_guard<std::mutex> lk(g_dc_mu);
  if (idx == 0 || idx > (u32)g_dcs.size() || !g_dcs[idx - 1].used) return 0;
  g_dcs[idx - 1].used = false;
  g_dcs[idx - 1].selected = nullptr;
  return 1;
}

int trt_user32_dc_is_valid(void* hdc) {
  const u32 h = (u32)(uintptr_t)hdc;
  if ((h & 0xFFFF0000u) != USER32_DC_TAG) return 0;
  const u32 idx = h & 0xFFFFu;
  std::lock_guard<std::mutex> lk(g_dc_mu);
  return idx > 0 && idx <= (u32)g_dcs.size() && g_dcs[idx - 1].used;
}

void* trt_user32_dc_selected(void* hdc) {
  const u32 h = (u32)(uintptr_t)hdc;
  if ((h & 0xFFFF0000u) != USER32_DC_TAG) return nullptr;
  const u32 idx = h & 0xFFFFu;
  std::lock_guard<std::mutex> lk(g_dc_mu);
  if (idx == 0 || idx > (u32)g_dcs.size() || !g_dcs[idx - 1].used) return nullptr;
  return g_dcs[idx - 1].selected;
}

void trt_user32_dc_set_selected(void* hdc, void* hobj) {
  const u32 h = (u32)(uintptr_t)hdc;
  if ((h & 0xFFFF0000u) != USER32_DC_TAG) return;
  const u32 idx = h & 0xFFFFu;
  std::lock_guard<std::mutex> lk(g_dc_mu);
  if (idx == 0 || idx > (u32)g_dcs.size() || !g_dcs[idx - 1].used) return;
  g_dcs[idx - 1].selected = hobj;
}

// ---- user32 GetDC/ReleaseDC on the record space ----
void* TRT_STDCALL trt_GetDC(void* hwnd) {
  (void)hwnd;
  void* hdc = trt_user32_dc_alloc();
  if (!hdc) log_line("user32: GetDC DC table full -> NULL");
  return hdc;  // a real DC record (window-less DCs are fine for records)
}

int TRT_STDCALL trt_ReleaseDC(void* hwnd, void* hdc) {
  (void)hwnd;
  return trt_user32_dc_release(hdc);
}

// ---- STAGE 7: UnityCrashHandler64 dialog/rect/icon surface ----
// Honest-minimal: dialogs do not exist offline (no message-loop host for
// DLGTEMPLATE), rect math is real, icons are record handles.
namespace {
struct rect16 { i32 l, t, r, b; };
} // namespace

void* TRT_STDCALL trt_AdjustWindowRect(void* rect, u32 style, int menu) {
  (void)style; (void)menu;
  if (!rect) return nullptr;
  log_line("user32: AdjustWindowRect -> identity (record window)");
  return (void*)(uintptr_t)1;
}

void* TRT_STDCALL trt_GetDlgItem(void* dlg, int id) {
  char line[96];
  snprintf(line, sizeof(line),
           "user32: GetDlgItem(dlg=%p id=%d) -> NULL (no dialog host)", dlg, id);
  log_line(line);
  return nullptr;
}

void* TRT_STDCALL trt_SendDlgItemMessageA(void* dlg, int id, u32 msg, u64 wp,
                                          i64 lp) {
  char line[128];
  snprintf(line, sizeof(line),
           "user32: SendDlgItemMessageA(dlg=%p id=%d msg=0x%x) -> 0 (no "
           "dialog host)", dlg, id, msg);
  log_line(line);
  return nullptr;
}

void* TRT_STDCALL trt_DialogBoxParamA(void* inst, const char* tmpl, void* parent,
                                      void* proc, i64 init) {
  char line[160];
  snprintf(line, sizeof(line),
           "user32: DialogBoxParamA(tmpl=%s) -> -1 (no dialog host; the "
           "crash handler UI is a named gap)",
           tmpl ? tmpl : "(ord)");
  log_line(line);
  (void)inst; (void)parent; (void)proc; (void)init;
  return (void*)(uintptr_t)-1;  // -1 = dialog creation failed (IDCANCEL won't do)
}

int TRT_STDCALL trt_EndDialog(void* dlg, i64 result) {
  (void)dlg; (void)result;
  return 1;
}

void* TRT_STDCALL trt_GetWindowLongA(void* hwnd, int idx) {
  return (void*)(uintptr_t)trt_GetWindowLongPtrW((u64)(uintptr_t)hwnd, idx);
}

i64 TRT_STDCALL trt_SetWindowTextW(void* hwnd, const u16* text) {
  char utf8[128];
  if (text) trt_utf16_to_utf8(text, utf8, sizeof(utf8));
  else snprintf(utf8, sizeof(utf8), "(null)");
  char line[192];
  snprintf(line, sizeof(line), "user32: SetWindowTextW(%p,'%s') -> TRUE", hwnd,
           utf8);
  log_line(line);
  return 1;
}

int TRT_STDCALL trt_OffsetRect(void* rect, int dx, int dy) {
  if (!rect) return 0;
  rect16* r = (rect16*)rect;
  r->l += dx; r->r += dx; r->t += dy; r->b += dy;
  return 1;
}

int TRT_STDCALL trt_InflateRect(void* rect, int dx, int dy) {
  if (!rect) return 0;
  rect16* r = (rect16*)rect;
  r->l -= dx; r->r += dx; r->t -= dy; r->b += dy;
  return 1;
}

int TRT_STDCALL trt_UnionRect(void* out, const void* a, const void* b) {
  if (!out || !a || !b) return 0;
  const rect16* ra = (const rect16*)a;
  const rect16* rb = (const rect16*)b;
  rect16* o = (rect16*)out;
  o->l = ra->l < rb->l ? ra->l : rb->l;
  o->t = ra->t < rb->t ? ra->t : rb->t;
  o->r = ra->r > rb->r ? ra->r : rb->r;
  o->b = ra->b > rb->b ? ra->b : rb->b;
  return (o->l < o->r && o->t < o->b) ? 1 : 0;  // empty union -> FALSE
}

void* TRT_STDCALL trt_GetIconInfo(void* icon, void* info) {
  (void)icon;
  log_line("user32: GetIconInfo -> FALSE (icon records carry no bitmap)");
  return (void*)(uintptr_t)0;
}

void* TRT_STDCALL trt_CreateIconFromResourceEx(const u8* bits, u32 len, int icon,
                                               u32 ver, int w, int h, u32 flags) {
  (void)bits; (void)len; (void)icon; (void)ver; (void)w; (void)h; (void)flags;
  log_line("user32: CreateIconFromResourceEx -> NULL (no GDI icon decode)");
  return nullptr;
}

void* TRT_STDCALL trt_LoadImageA(void* inst, const char* name, u32 type, int cx,
                                 int cy, u32 flags) {
  char line[160];
  snprintf(line, sizeof(line),
           "user32: LoadImageA(inst=%p name=%s type=%u %dx%d) -> NULL (no "
           "image decode offline)", inst, name ? name : "(null)", type, cx, cy);
  log_line(line);
  return nullptr;
}

void* TRT_STDCALL trt_SetForegroundWindow(void* hwnd) {
  (void)hwnd;
  return (void*)(uintptr_t)1;  // TRUE: the record window is always foreground
}

int TRT_STDCALL trt_LookupIconIdFromDirectoryEx(const u8* bits, int icon,
                                                int w, int h, u32 flags) {
  (void)bits; (void)icon; (void)flags;
  // pick the entry closest to the requested size: v0 returns the first
  // (16x16 default is the smallest legitimate answer)
  (void)w; (void)h;
  return 1;
}

static const trt_named_fn k_user32_fns[] = {
  {"PostQuitMessage", (void*)&trt_PostQuitMessage},
  {"SetFocus", (void*)&trt_SetFocus},
  {"GetFocus", (void*)&trt_GetFocus},
  {"SetWindowPos", (void*)&trt_SetWindowPos},
  {"IsWindow", (void*)&trt_IsWindow},
  {"IsWindowVisible", (void*)&trt_IsWindowVisible},
  {"GetClientRect", (void*)&trt_GetClientRect},
  {"SetTimer", (void*)&trt_SetTimer},
  {"KillTimer", (void*)&trt_KillTimer},
  {"RegisterClassExW", (void*)&trt_RegisterClassExW},
  {"CreateWindowExW", (void*)&trt_CreateWindowExW},
  {"DestroyWindow", (void*)&trt_DestroyWindow},
  {"ShowWindow", (void*)&trt_ShowWindow},
  {"UpdateWindow", (void*)&trt_UpdateWindow},
  {"DefWindowProcW", (void*)&trt_DefWindowProcW},
  {"GetActiveWindow", (void*)&trt_GetActiveWindow},
  {"GetMessageW", (void*)&trt_GetMessageW},
  {"PeekMessageW", (void*)&trt_PeekMessageW},
  {"PostMessageW", (void*)&trt_PostMessageW},
  {"TranslateMessage", (void*)&trt_TranslateMessage},
  {"DispatchMessageW", (void*)&trt_DispatchMessageW},
  {"GetAsyncKeyState", (void*)&trt_GetAsyncKeyState},
  {"GetKeyState", (void*)&trt_GetKeyState},
  {"GetKeyboardState", (void*)&trt_GetKeyboardState},
  {"GetCursorPos", (void*)&trt_GetCursorPos},
  {"SetCursorPos", (void*)&trt_SetCursorPos},
  {"ClipCursor", (void*)&trt_ClipCursor},
  {"GetSystemMetrics", (void*)&trt_GetSystemMetrics},
  {"LoadCursorW", (void*)&trt_LoadCursorW},
  {"LoadIconW", (void*)&trt_LoadIconW},
  {"RegisterRawInputDevices", (void*)&trt_RegisterRawInputDevices},
  {"GetRawInputData", (void*)&trt_GetRawInputData},
  {"MapVirtualKeyW", (void*)&trt_MapVirtualKeyW},
  {"ShowCursor", (void*)&trt_ShowCursor},
  {"SetCapture", (void*)&trt_SetCapture},
  {"ReleaseCapture", (void*)&trt_ReleaseCapture},
  {"GetCapture", (void*)&trt_GetCapture},
  {"GetForegroundWindow", (void*)&trt_GetForegroundWindow},
  {"GetKeyboardLayout", (void*)&trt_GetKeyboardLayout},
  {"SendMessageW", (void*)&trt_SendMessageW},
  {"GetDC", (void*)&trt_GetDC},
  {"ReleaseDC", (void*)&trt_ReleaseDC},
  // ---- STAGE 7: UnityCrashHandler64 surface ----
  {"AdjustWindowRect", (void*)&trt_AdjustWindowRect},
  {"GetDlgItem", (void*)&trt_GetDlgItem},
  {"SendDlgItemMessageA", (void*)&trt_SendDlgItemMessageA},
  {"DialogBoxParamA", (void*)&trt_DialogBoxParamA},
  {"EndDialog", (void*)&trt_EndDialog},
  {"GetWindowLongA", (void*)&trt_GetWindowLongA},
  {"SetWindowTextW", (void*)&trt_SetWindowTextW},
  {"OffsetRect", (void*)&trt_OffsetRect},
  {"InflateRect", (void*)&trt_InflateRect},
  {"UnionRect", (void*)&trt_UnionRect},
  {"GetIconInfo", (void*)&trt_GetIconInfo},
  {"CreateIconFromResourceEx", (void*)&trt_CreateIconFromResourceEx},
  {"LoadImageA", (void*)&trt_LoadImageA},
  {"SetForegroundWindow", (void*)&trt_SetForegroundWindow},
  {"LookupIconIdFromDirectoryEx", (void*)&trt_LookupIconIdFromDirectoryEx},
};

int trt_bridge_register_user32(void) {
  trt_module_register("user32.dll", TRT_ROUTE_BUILTIN, k_user32_fns,
                      (int)(sizeof(k_user32_fns) / sizeof(k_user32_fns[0])));
  // the OrTMM-audit gap batch (window longs, message pump, UI honesty)
  {
    extern void trt_u32_gap_register(void);
    trt_u32_gap_register();
  }
  return 0;
}

} // extern "C"
