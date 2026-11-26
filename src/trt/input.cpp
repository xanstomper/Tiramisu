// tiramisu/src/trt/input.cpp
#include "input.h"
#include "trt_trace.h"
#include <cstdio>

#include <math.h>
#include <mutex>
#include <string.h>

namespace {

struct input_state {
  // keyboard
  u8  down[TRT_MAX_VK];      // current
  u8  pressed[TRT_MAX_VK];   // transitioned down this frame
  u8  toggle[TRT_MAX_VK];    // toggled bit0 (Caps/Num/Scroll)
  u16 scan[TRT_MAX_VK];      // last scan code per VK (games read lParam)
  u8 extended[TRT_MAX_VK];   // extended scan flag for synthesized releases
  u16 last_vk;               // for GetAsyncKeyState bit15
  u32 char_q[64]; int char_count;

  // mouse
  int mx, my;                // virtual-cursor position
  int last_abs_x, last_abs_y;
  bool abs_init;             // independent of SetCursorPos to prevent spikes
  int dx_acc, dy_acc;        // accumulated deltas since last consume
  bool mouse_init;           // first absolute event positions, no delta
  int rdx, rdy;              // last REAL mouse deltas (MouseDevice)
  u8  mbtn;                  // bit0 L, bit1 R, bit2 M, bit3 X1, bit4 X2
  i32 wheel_acc;

  // pads 0..3 (XINPUT_GAMEPAD_* bits mirrored directly)
  struct pad_slot {
    bool connected;
    bool conn_known;
    i16 lx, ly, rx, ry;
    u16 pbtn;
    u8  tl, tr;
    u16 vib_left, vib_right;
    bool vib_dirty;
  } pads[4];

  // display
  int dw, dh;

  // cursor visibility hook (ShowCursor: default visible => hidden=false)
  bool cursor_hidden;
  bool cursor_shape_hidden;
  int cursor_show_count;
  bool cursor_force_visible;
  bool mouse_pointer;
  bool mouse_pointer_new;

  // raw input: registered devices + a ring of RAWINPUT blobs
  u8  raw_mouse_reg, raw_kbd_reg;
  u64 raw_mouse_window, raw_kbd_window;
  u64 raw_next_handle;
  struct raw_rec { u64 handle; u64 window; u8 blob[48]; u32 size; };
  raw_rec raw_ring[TRT_MSG_RING];

  // message ring
  trt_msg ring[TRT_MSG_RING];
  int ring_head, ring_count;
};

input_state g;
// Host CoreWindow UI thread produces input while the guest runs on a worker
// thread; recursive_mutex keeps internal trt_msg_post / trt_raw_post safe.
std::recursive_mutex g_input_mu;

} // namespace

extern "C" {
// Settings knob (input_deadzone, 0..32767); weak symbol keeps offline tests
// green when src/host/settings.cpp is not in the test binary. Missing
// definition resolves to 0 -> clamp_axis falls back to the default 8000.
int trt_settings_input_deadzone(void) __attribute__((weak));

int clamp_axis(i16 v) {  // deadzone from settings (default ~8k), normalize
  // Weak symbol may be null in test binaries without settings.cpp.
  const int dz_set = trt_settings_input_deadzone ? trt_settings_input_deadzone() : 0;
  const int dz = (dz_set > 0 && dz_set <= 20000) ? dz_set : 8000;
  int a = v;
  if (a > dz) a -= dz; else if (a < -dz) a += dz; else return 0;
  return (int)(a * 100 / (32767 - dz));
}

// raw-blob feeders (defined below; used by the host-feed section)
u32 TRT_STDCALL trt_MapVirtualKeyW(u32 code, u32 type);
void trt_raw_post_mouse(i32 dx, i32 dy, u16 btn_flags, u16 btn_data);
void trt_raw_post_key(u16 vk, u16 scan, bool down, u8 extended);

static bool g_clip_active = false;
static i32 g_clip_l = 0, g_clip_t = 0, g_clip_r = 0, g_clip_b = 0;

static u64 compute_mk_wparam() {
  u64 mk = 0;
  if (g.mbtn & (1u << 0)) mk |= 0x0001u;  // MK_LBUTTON
  if (g.mbtn & (1u << 1)) mk |= 0x0002u;  // MK_RBUTTON
  if (g.down[0x10])       mk |= 0x0004u;  // MK_SHIFT
  if (g.down[0x11])       mk |= 0x0008u;  // MK_CONTROL
  if (g.mbtn & (1u << 2)) mk |= 0x0010u;  // MK_MBUTTON
  if (g.mbtn & (1u << 3)) mk |= 0x0020u;  // MK_XBUTTON1
  if (g.mbtn & (1u << 4)) mk |= 0x0040u;  // MK_XBUTTON2
  return mk;
}

static void clamp_to_clip(int* x, int* y) {
  if (!g_clip_active || g_clip_r <= g_clip_l || g_clip_b <= g_clip_t) return;
  if (*x < g_clip_l) *x = g_clip_l;
  else if (*x >= g_clip_r) *x = g_clip_r - 1;
  if (*y < g_clip_t) *y = g_clip_t;
  else if (*y >= g_clip_b) *y = g_clip_b - 1;
}

static void sync_modifier_vk(u16 vk, bool down, u16 scan, u8 extended) {
  static constexpr struct { u16 gen; u16 left; u16 right; } kMods[] = {
      {0x10 /*VK_SHIFT*/,   0xA0 /*VK_LSHIFT*/,   0xA1 /*VK_RSHIFT*/},
      {0x11 /*VK_CONTROL*/, 0xA2 /*VK_LCONTROL*/, 0xA3 /*VK_RCONTROL*/},
      {0x12 /*VK_MENU*/,    0xA4 /*VK_LMENU*/,    0xA5 /*VK_RMENU*/},
  };
  for (const auto& m : kMods) {
    if (vk == m.left || vk == m.right) {
      const bool any = (g.down[m.left] || g.down[m.right]);
      if (any && !g.down[m.gen]) g.pressed[m.gen] = 1;
      g.down[m.gen] = any ? 1 : 0;
      return;
    }
    if (vk == m.gen) {
      const bool is_right = (vk == 0x10) ? (scan == 0x36 || extended != 0)
                                         : (extended != 0);
      const u16 side = is_right ? m.right : m.left;
      if (down) {
        if (!g.down[side]) g.pressed[side] = 1;
        g.down[side] = 1;
      } else {
        g.down[side] = 0;
        if (!scan && !extended) {
          g.down[m.left] = 0;
          g.down[m.right] = 0;
        }
      }
      return;
    }
  }
}

constexpr u32 k_pointer_update = 0x0245u;
constexpr u32 k_pointer_down = 0x0246u;
constexpr u32 k_pointer_up = 0x0247u;
constexpr u32 k_pointer_wheel = 0x024Eu;
thread_local trt_mouse_pointer t_mouse_pointer{};

static bool post_pointer_locked(u32 message, u32 change = 0, i32 wheel = 0) {
  if (g.ring_count >= TRT_MSG_RING) return false;
  const u32 flags = 0x2002u /*PRIMARY | INRANGE*/ |
                    (g.mbtn ? 4u /*INCONTACT*/ : 0u) |
                    (static_cast<u32>(g.mbtn) << 4) |
                    (g.mouse_pointer_new ? 1u /*NEW*/ : 0u);
  const u32 high = message == k_pointer_wheel ? static_cast<u16>(wheel) : flags;
  const i64 pos = static_cast<i64>((static_cast<u32>(g.my) << 16) |
                                   (static_cast<u32>(g.mx) & 0xFFFFu));
  const trt_msg msg{message, (high << 16) | 1u, pos, flags, change};
  if (message == k_pointer_update && !change && g.ring_count && !g.mouse_pointer_new) {
    auto& last = g.ring[(g.ring_head + g.ring_count - 1) % TRT_MSG_RING];
    if (last.message == k_pointer_update && !last.pointer_change && last.pointer_flags == flags) {
      last = msg;
      return true;
    }
  }
  g.ring[(g.ring_head + g.ring_count++) % TRT_MSG_RING] = msg;
  g.mouse_pointer_new = false;
  return true;
}

void trt_input_enable_mouse_pointer(bool enable) {
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  if (enable && !g.mouse_pointer) g.mouse_pointer_new = true;
  g.mouse_pointer = enable;
}

bool trt_input_mouse_pointer_enabled(void) {
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  return g.mouse_pointer;
}

void trt_input_accept_pointer(const trt_msg* msg, u64 window, u32 time) {
  if (!msg->pointer_flags) return;
  t_mouse_pointer.x = static_cast<i16>(msg->lparam);
  t_mouse_pointer.y = static_cast<i16>(static_cast<u64>(msg->lparam) >> 16);
  t_mouse_pointer.flags = msg->pointer_flags;
  t_mouse_pointer.time = time;
  t_mouse_pointer.window = window;
  t_mouse_pointer.change = msg->pointer_change;
  t_mouse_pointer.wheel = 0;
  {
    std::lock_guard<std::recursive_mutex> lk(g_input_mu);
    t_mouse_pointer.key_states = (g.down[0x10] ? 4u : 0u) |
                                 (g.down[0x11] ? 8u : 0u);
  }
  ++t_mouse_pointer.frame;
  switch (msg->message) {
    case k_pointer_down: t_mouse_pointer.flags |= 0x10000u; break;
    case k_pointer_up: t_mouse_pointer.flags |= 0x40000u; break;
    case k_pointer_wheel:
      t_mouse_pointer.flags |= 0x80000u;
      t_mouse_pointer.wheel = static_cast<i16>(msg->wparam >> 16);
      break;
    default: t_mouse_pointer.flags |= 0x20000u; break;
  }
}

bool trt_input_get_mouse_pointer(u32 id, trt_mouse_pointer* out) {
  if (id != 1 || !out || !t_mouse_pointer.frame) return false;
  *out = t_mouse_pointer;
  return true;
}

static void post_mouse_move_locked() {
  if (g.mouse_pointer) {
    post_pointer_locked(k_pointer_update);
  } else {
    trt_msg_post(0x0200 /*WM_MOUSEMOVE*/, compute_mk_wparam(),
                 (i64)(((u32)g.my << 16) | ((u32)g.mx & 0xFFFFu)));
  }
}

// ---------------- host feed ----------------
void trt_input_key_event(u16 vk, bool down) {
  trt_input_key_event2(vk, down, 0, 0, 0);
}

void trt_input_key_event2(u16 vk, bool down, u16 scan, u8 extended, u8 repeat) {
  if (vk >= TRT_MAX_VK) return;
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  const bool previous = g.down[vk] != 0;
  if (down && !previous) {
    g.pressed[vk] = 1;
    if (vk == 0x14 /*VK_CAPITAL*/ || vk == 0x90 /*VK_NUMLOCK*/ || vk == 0x91 /*VK_SCROLL*/)
      g.toggle[vk] ^= 1u;
  }
  g.down[vk] = down ? 1 : 0;
  if (scan == 0) {
    const u32 mv = trt_MapVirtualKeyW(vk, 4 /*MAPVK_VK_TO_VSC_EX*/);
    if (mv) {
      scan = (u16)(mv & 0xFFu);
      if ((mv & 0xFF00u) == 0xE000u) extended = 1;
    }
  }
  if (scan) {
    g.scan[vk] = scan;
    g.extended[vk] = extended;
  } else {
    scan = g.scan[vk];
    extended = g.extended[vk];
  }
  sync_modifier_vk(vk, down, scan, extended);
  if (down) g.last_vk = vk;
  // WM_KEYDOWN/UP lParam: repeat(16) scan(9)<<16 ext(1)<<24 prev(1)<<30 up(1)<<31
  const u32 lp = 1u | ((u32)(scan & 0xFFu) << 16) |
                 ((u32)(extended != 0) << 24) |
                 ((!down || previous || repeat) ? 0x40000000u : 0u) |
                 (down ? 0u : 0x80000000u);
  trt_msg_post(down ? 0x0100 /*WM_KEYDOWN*/ : 0x0101 /*WM_KEYUP*/, vk,
               (i64)(u64)lp);
  if (g.raw_kbd_reg) trt_raw_post_key(vk, scan, down, extended);
}

void trt_input_char_event(u32 ch) {
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  if (g.char_count < 64) g.char_q[g.char_count++] = ch;
  trt_msg_post(0x0102 /*WM_CHAR*/, ch, 0);
}

void trt_input_mouse_delta_raw(i32 dx, i32 dy) {
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  // real mouse (MouseDevice.MouseMoved): feed BOTH consumers
  g.rdx = dx;
  g.rdy = dy;
  g.dx_acc += dx;  // GetCursorPos-based games + WM_MOUSEMOVE keep working
  g.dy_acc += dy;
  const int w = g.dw ? g.dw : 1920;
  const int h = g.dh ? g.dh : 1080;
  g.mx += dx;
  g.my += dy;
  if (g_clip_active && g_clip_r > g_clip_l && g_clip_b > g_clip_t) {
    clamp_to_clip(&g.mx, &g.my);
  } else {
    if (g.mx < 0) g.mx = 0; else if (g.mx >= w) g.mx = w - 1;
    if (g.my < 0) g.my = 0; else if (g.my >= h) g.my = h - 1;
  }
  g.mouse_init = true;
  // synthesize WM_INPUT for engines on the Raw Input path
  if (g.raw_mouse_reg) trt_raw_post_mouse(dx, dy, 0, 0);
  if (dx || dy) post_mouse_move_locked();
}

void trt_input_set_cursor_pos(int x, int y) {
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  // SetCursorPos clamps to the desktop first, then to the clip rect, and
  // notifies the window of the move. Guests that warp the pointer and then
  // wait for WM_MOUSEMOVE (camera drag, RTS edge scroll) need the message,
  // not just the stored position.
  const int w = g.dw ? g.dw : 1920;
  const int h = g.dh ? g.dh : 1080;
  if (x < 0) x = 0; else if (x >= w) x = w - 1;
  if (y < 0) y = 0; else if (y >= h) y = h - 1;
  clamp_to_clip(&x, &y);
  const bool moved = !g.mouse_init || x != g.mx || y != g.my;
  g.mx = x;
  g.my = y;
  g.mouse_init = true;
  if (moved) post_mouse_move_locked();
}

void trt_input_set_clip_rect(bool enable, i32 left, i32 top, i32 right, i32 bottom) {
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  if (!enable || right <= left || bottom <= top) {
    g_clip_active = false;
    g_clip_l = g_clip_t = g_clip_r = g_clip_b = 0;
    return;
  }
  g_clip_active = true;
  g_clip_l = left;
  g_clip_t = top;
  g_clip_r = right;
  g_clip_b = bottom;
  clamp_to_clip(&g.mx, &g.my);
}

bool trt_input_get_clip_rect(i32* left, i32* top, i32* right, i32* bottom) {
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  if (!g_clip_active) return false;
  if (left) *left = g_clip_l;
  if (top) *top = g_clip_t;
  if (right) *right = g_clip_r;
  if (bottom) *bottom = g_clip_b;
  return true;
}

void trt_input_pointer_move(int x, int y, bool suppress_delta) {
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  if (!suppress_delta) {
    if (!g.abs_init) {
      g.abs_init = true;
      g.last_abs_x = x;
      g.last_abs_y = y;
    } else {
      const int dx = x - g.last_abs_x;
      const int dy = y - g.last_abs_y;
      g.dx_acc += dx;
      g.dy_acc += dy;
      if (g.raw_mouse_reg && (dx || dy)) trt_raw_post_mouse(dx, dy, 0, 0);
      g.last_abs_x = x;
      g.last_abs_y = y;
    }
  } else {
    g.last_abs_x = x;
    g.last_abs_y = y;
    g.abs_init = true;
  }
  // When hardware raw mouse stream owns a locked/hidden cursor, do not snap
  // g.mx/g.my back to screen-edge PointerMoved coordinates.
  if (suppress_delta && (g_clip_active || g.cursor_hidden)) {
    return;
  }
  clamp_to_clip(&x, &y);
  if (!g.mouse_init) {
    g.mouse_init = true;
    g.mx = x;
    g.my = y;
    if (suppress_delta) return;
    post_mouse_move_locked();
    return;
  }
  g.mx = x;
  g.my = y;
  post_mouse_move_locked();
}

void trt_input_mouse_move(int x, int y) {
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  const bool was_init = g.mouse_init;
  trt_input_pointer_move(x, y, false);
  (void)was_init;
}

void trt_input_mouse_button(int btn, bool down) {
  if (btn < 0 || btn > 4) return;
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  static const u16 k_btn_vk[5] = {
      0x01u /*VK_LBUTTON*/, 0x02u /*VK_RBUTTON*/, 0x04u /*VK_MBUTTON*/,
      0x05u /*VK_XBUTTON1*/, 0x06u /*VK_XBUTTON2*/};
  const u16 vk = k_btn_vk[btn];
  if (down) {
    if (!g.down[vk]) g.pressed[vk] = 1;
    g.down[vk] = 1;
  } else {
    g.down[vk] = 0;
  }
  const u8 bit = (u8)(1u << btn);
  const u8 old_buttons = g.mbtn;
  if (down) g.mbtn |= bit; else g.mbtn &= (u8)~bit;

  const u64 mk = compute_mk_wparam();
  const i64 lp = (i64)(((u32)g.my << 16) | ((u32)g.mx & 0xFFFFu));
  if (g.mouse_pointer) {
    const u32 message = !old_buttons && g.mbtn ? k_pointer_down
                      : old_buttons && !g.mbtn ? k_pointer_up : k_pointer_update;
    post_pointer_locked(message, 1u + 2u * static_cast<u32>(btn) + (down ? 0u : 1u));
  } else if (btn <= 2) {
    const u32 msg = btn == 0 ? (down ? 0x0201u : 0x0202u)
                  : btn == 1 ? (down ? 0x0204u : 0x0205u)
                             : (down ? 0x0207u : 0x0208u);
    trt_msg_post(msg, mk, lp);
  } else {
    const u32 msg = down ? 0x020Bu /*WM_XBUTTONDOWN*/ : 0x020Cu /*WM_XBUTTONUP*/;
    const u64 xbtn = (btn == 3) ? 1ULL : 2ULL;
    trt_msg_post(msg, (xbtn << 16) | mk, lp);
  }
  if (g.raw_mouse_reg) {
    static const u16 k_ri_down[5] = {0x0001u, 0x0004u, 0x0010u, 0x0040u, 0x0100u};
    static const u16 k_ri_up[5]   = {0x0002u, 0x0008u, 0x0020u, 0x0080u, 0x0200u};
    trt_raw_post_mouse(0, 0, down ? k_ri_down[btn] : k_ri_up[btn], 0);
  }
}

void trt_input_wheel(i32 delta) {
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  g.wheel_acc += delta;
  const u64 wp = ((u64)(u16)(i16)delta << 16) | compute_mk_wparam();
  const i64 lp = (i64)(((u32)g.my << 16) | ((u32)g.mx & 0xFFFFu));
  if (g.mouse_pointer) post_pointer_locked(k_pointer_wheel, 0, delta);
  else trt_msg_post(0x020A /*WM_MOUSEWHEEL*/, wp, lp);
  if (g.raw_mouse_reg) {
    trt_raw_post_mouse(0, 0, 0x0400u /*RI_MOUSE_WHEEL*/, (u16)(i16)delta);
  }
}

void trt_input_pad_state_n(u32 index, bool connected, i16 lx, i16 ly,
                           i16 rx, i16 ry, u16 buttons, u8 tl, u8 tr) {
  if (index >= 4) return;
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  g.pads[index].connected = connected;
  g.pads[index].conn_known = true;
  if (connected) {
    g.pads[index].lx = lx; g.pads[index].ly = ly;
    g.pads[index].rx = rx; g.pads[index].ry = ry;
    g.pads[index].pbtn = buttons;
    g.pads[index].tl = tl; g.pads[index].tr = tr;
  } else {
    g.pads[index].lx = g.pads[index].ly = 0;
    g.pads[index].rx = g.pads[index].ry = 0;
    g.pads[index].pbtn = 0;
    g.pads[index].tl = g.pads[index].tr = 0;
  }
}

void trt_input_pad_state(i16 lx, i16 ly, i16 rx, i16 ry,
                         u16 buttons, u8 tl, u8 tr) {
  trt_input_pad_state_n(0, true, lx, ly, rx, ry, buttons, tl, tr);
}

void trt_input_display_size(int w, int h) {
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  g.dw = w; g.dh = h;
}

void trt_input_frame_begin() {
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  memset(g.pressed, 0, sizeof(g.pressed));
  g.char_count = 0;
}

// ---------------- guest read ----------------
bool trt_key_down(u16 vk) {
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  return vk < TRT_MAX_VK && g.down[vk] != 0;
}

i16 trt_key_async(u16 vk) {
  if (vk >= TRT_MAX_VK) return 0;
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  return (i16)((g.down[vk] ? -0x8000 : 0) | (g.pressed[vk] ? 1 : 0));
}

i16 trt_key_sync(u16 vk) {
  if (vk >= TRT_MAX_VK) return 0;
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  const i16 tog = (vk == 0x14 || vk == 0x90 || vk == 0x91) ? (i16)(g.toggle[vk] & 1u)
                                                            : (i16)(g.pressed[vk] ? 1 : 0);
  return (i16)((g.down[vk] ? -0x8000 : 0) | tog);
}

int trt_mouse_pos(int* x, int* y) {
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  if (x) *x = g.mx;
  if (y) *y = g.my;
  return g.mbtn;
}

void trt_mouse_delta(int* dx, int* dy) {
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  if (dx) *dx = g.dx_acc;
  if (dy) *dy = g.dy_acc;
  g.dx_acc = 0;
  g.dy_acc = 0;
}

void trt_mouse_delta_raw(int* dx, int* dy) {
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  if (dx) *dx = g.rdx;
  if (dy) *dy = g.rdy;
}

bool trt_mouse_button(int btn) {
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  return (btn >= 0 && btn <= 4) && (g.mbtn & (1u << btn)) != 0;
}

i32 trt_mouse_wheel() {
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  const i32 w = g.wheel_acc;
  g.wheel_acc = 0;
  return w;
}

bool trt_pad_connected(u32 index) {
  if (index >= 4) return false;
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  if (index == 0 && !g.pads[0].conn_known) return true;
  return g.pads[index].connected;
}

bool trt_pad_down_n(u32 index, u16 bit) {
  if (index >= 4) return false;
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  return trt_pad_connected(index) && (g.pads[index].pbtn & bit) != 0;
}

bool trt_pad_down(u16 bit) { return trt_pad_down_n(0, bit); }

float trt_pad_stick(int s) {
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  const auto& p = g.pads[0];
  switch (s) {
    case 0: return (float)clamp_axis(p.lx) / 100.0f;
    case 1: return (float)clamp_axis(p.ly) / 100.0f;
    case 2: return (float)clamp_axis(p.rx) / 100.0f;
    case 3: return (float)clamp_axis(p.ry) / 100.0f;
  }
  return 0.0f;
}

i16 trt_pad_thumb_n(u32 index, int s) {
  if (index >= 4) return 0;
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  const auto& p = g.pads[index];
  switch (s) {
    case 0: return p.lx;
    case 1: return p.ly;
    case 2: return p.rx;
    case 3: return p.ry;
  }
  return 0;
}

i16 trt_pad_thumb(int s) { return trt_pad_thumb_n(0, s); }

u8 trt_pad_trigger_n(u32 index, int t) {
  if (index >= 4) return 0;
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  return t == 0 ? g.pads[index].tl : g.pads[index].tr;
}

u8 trt_pad_trigger(int t) { return trt_pad_trigger_n(0, t); }

void trt_pad_set_vibration(u32 index, u16 left_motor, u16 right_motor) {
  if (index >= 4) return;
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  if (g.pads[index].vib_left != left_motor ||
      g.pads[index].vib_right != right_motor) {
    g.pads[index].vib_left = left_motor;
    g.pads[index].vib_right = right_motor;
    g.pads[index].vib_dirty = true;
  }
}

namespace {

// Host-settings hooks (weak: absent when trt runs without the host layer —
// offline tests). File-scope because clang rejects attributed block-scope
// extern declarations.
extern "C" bool trt_settings_pad_vibration() __attribute__((weak));

} // namespace

bool trt_pad_get_vibration(u32 index, u16* left_motor, u16* right_motor) {
  if (index >= 4) return false;
  // host kill-switch (dashboard setting): swallow guest rumble requests.
  if (trt_settings_pad_vibration && !trt_settings_pad_vibration()) {
    if (left_motor) *left_motor = 0;
    if (right_motor) *right_motor = 0;
    g.pads[index].vib_dirty = false;
    return true;  // consumed, silently zeroed (user chose no vibration)
  }
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  const bool dirty = g.pads[index].vib_dirty;
  if (left_motor) *left_motor = g.pads[index].vib_left;
  if (right_motor) *right_motor = g.pads[index].vib_right;
  g.pads[index].vib_dirty = false;
  return dirty;
}

int trt_display_size(int* w, int* h) {
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  if (w) *w = g.dw ? g.dw : 1920;
  if (h) *h = g.dh ? g.dh : 1080;
  return 0;
}

u16 trt_key_scan(u16 vk) {
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  return vk < TRT_MAX_VK ? g.scan[vk] : 0;
}

void trt_input_copy_dik_state(u8 out[256]) {
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  memset(out, 0, 256);
  for (u16 vk = 0; vk < TRT_MAX_VK; ++vk) {
    // The generic modifier states mirror the left and right states.
    if (!g.down[vk] || vk == 0x10 || vk == 0x11 || vk == 0x12) continue;
    u16 scan = g.scan[vk];
    bool extended = g.extended[vk] != 0;
    if (!scan) {
      const u32 mapped = trt_MapVirtualKeyW(vk, 4 /*MAPVK_VK_TO_VSC_EX*/);
      scan = static_cast<u16>(mapped & 0xff);
      extended = (mapped & 0xff00) == 0xe000;
    }
    if (!scan || scan >= 0x80) continue;

    // DirectInput uses bit 7 for E0 keys, with special indices for
    // Pause (E1) and Num Lock.
    u16 dik = scan | (extended ? 0x80 : 0);
    if (vk == 0x13 /*VK_PAUSE*/) dik = 0xc5;
    if (vk == 0x90 /*VK_NUMLOCK*/) dik = 0x45;
    out[dik] = 0x80;
  }
}

bool trt_cursor_snapshot(int* x, int* y) {
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  *x = g.mouse_init ? g.mx : (g.dw ? g.dw : 1920) / 2;
  *y = g.mouse_init ? g.my : (g.dh ? g.dh : 1080) / 2;
  const bool pinned = g_clip_active &&
      (g_clip_r - g_clip_l <= 1 || g_clip_b - g_clip_t <= 1);
  return (g.cursor_force_visible || (!g.cursor_hidden && !g.cursor_shape_hidden)) && !pinned;
}

int trt_input_show_cursor(bool show) {
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  g.cursor_show_count += show ? 1 : -1;
  g.cursor_hidden = g.cursor_show_count < 0;
  return g.cursor_show_count;
}

void trt_input_set_cursor_shape(bool visible) {
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  g.cursor_shape_hidden = !visible;
}

void trt_input_reset_cursor_mode(bool force_visible) {
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  g.cursor_show_count = 0;
  g.cursor_force_visible = force_visible;
  g.cursor_hidden = false;
  g.cursor_shape_hidden = false;
  g.mouse_pointer = false;
  g.mouse_pointer_new = false;
  g_clip_active = false;
  g.abs_init = false;
}

void trt_input_release_all(void) {
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  for (unsigned btn = 0; btn < 5; ++btn)
    if (g.mbtn & (1u << btn)) trt_input_mouse_button(btn, false);
  // Release handed modifiers before generic aliases so their physical
  // scan codes reach Raw Input when focus changes.
  for (u16 vk = 0xA0; vk <= 0xA5; ++vk) {
    if (g.down[vk]) trt_input_key_event2(vk, false, g.scan[vk], g.extended[vk], 0);
  }
  for (u16 vk = 8; vk < TRT_MAX_VK; ++vk) {
    if (g.down[vk]) trt_input_key_event2(vk, false, g.scan[vk], g.extended[vk], 0);
  }
  // A pad or mouse button held when the console suspends the app must not
  // look held after resume: the host re-polls the real device on the way
  // back, so clearing here is the truthful state in between.
  for (auto& p : g.pads) {
    p.pbtn = 0;
    p.tl = 0;
    p.tr = 0;
    p.lx = 0;
    p.ly = 0;
    p.rx = 0;
    p.ry = 0;
  }
  memset(g.pressed, 0, sizeof(g.pressed));
  g.mbtn = 0;
  g.wheel_acc = 0;
  g.dx_acc = 0;
  g.dy_acc = 0;
  g.rdx = 0;
  g.rdy = 0;
  g.char_count = 0;
  g.abs_init = false;
}

void trt_set_cursor_visible(bool v) {
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  g.cursor_hidden = !v;
}

// ---------------- raw input (WM_INPUT path) ----------------
// RAWINPUT blob: RAWINPUTHEADER(24) + RAWMOUSE(24) or RAWKEYBOARD(16)
void trt_raw_post_mouse(i32 dx, i32 dy, u16 btn_flags, u16 btn_data) {
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  const u64 h = g.raw_next_handle + 1;
  if (!trt_msg_post(0x00FF /*WM_INPUT*/, 0, (i64)h)) return;
  g.raw_next_handle = h;
  input_state::raw_rec& r = g.raw_ring[(h - 1) % TRT_MSG_RING];
  r.handle = h;
  r.window = g.raw_mouse_window;
  r.size = 48;
  memset(r.blob, 0, sizeof(r.blob));
  const u32 type = 0;         // RIM_TYPEMOUSE
  const u32 size = 48;
  const u64 dev = 0x00060001ULL, wp = 0;  // hDevice, wParam RIM_INPUT
  const u16 flags = 0;        // MOUSE_MOVE_RELATIVE
  const u32 none = 0;         // ulRawButtons
  memcpy(r.blob + 0, &type, 4);
  memcpy(r.blob + 4, &size, 4);
  memcpy(r.blob + 8, &dev, 8);
  memcpy(r.blob + 16, &wp, 8);
  memcpy(r.blob + 24, &flags, 2);
  memcpy(r.blob + 28, &btn_flags, 2);  // usButtonFlags
  memcpy(r.blob + 30, &btn_data, 2);   // usButtonData
  memcpy(r.blob + 32, &none, 4);
  memcpy(r.blob + 36, &dx, 4);   // lLastX
  memcpy(r.blob + 40, &dy, 4);   // lLastY
}

void trt_raw_post_key(u16 vk, u16 scan, bool down, u8 extended) {
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  const u64 h = g.raw_next_handle + 1;
  if (!trt_msg_post(0x00FF /*WM_INPUT*/, 0, (i64)h)) return;
  g.raw_next_handle = h;
  input_state::raw_rec& r = g.raw_ring[(h - 1) % TRT_MSG_RING];
  r.handle = h;
  r.window = g.raw_kbd_window;
  r.size = 40;
  memset(r.blob, 0, sizeof(r.blob));
  const u32 type = 1;          // RIM_TYPEKEYBOARD
  const u32 size = 40;
  const u64 dev = 0x00060002ULL, wp = 0;  // hDevice, wParam RIM_INPUT
  const u16 make_code = scan ? scan : g.scan[vk < TRT_MAX_VK ? vk : 0];
  const u16 rflags = (u16)((down ? 0u : 1u /*RI_KEY_BREAK*/) |
                           (extended ? 2u /*RI_KEY_E0*/ : 0u));
  const u16 reserved = 0;
  const u16 vkey = vk;
  const u32 msg = down ? 0x0100u : 0x0101u;
  memcpy(r.blob + 0, &type, 4);
  memcpy(r.blob + 4, &size, 4);
  memcpy(r.blob + 8, &dev, 8);
  memcpy(r.blob + 16, &wp, 8);
  memcpy(r.blob + 24, &make_code, 2);  // MakeCode
  memcpy(r.blob + 26, &rflags, 2);     // Flags
  memcpy(r.blob + 28, &reserved, 2);   // Reserved
  memcpy(r.blob + 30, &vkey, 2);       // VKey
  memcpy(r.blob + 32, &msg, 4);        // Message
}

bool trt_raw_register(const void* devices, u32 count) {
  if (!devices || count == 0 || count > 8) return false;
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  // RAWINPUTDEVICE: usUsagePage(2) usUsage(2) dwFlags(4) hwndTarget(8)
  struct rid { u16 page; u16 usage; u32 flags; u64 hwnd; };
  const rid* d = (const rid*)devices;
  for (u32 i = 0; i < count; ++i) {
    const u8 reg = (d[i].flags & 0x00000001u /*RIDEV_REMOVE*/) ? 0 : 1;
    if (d[i].page != 1) continue;
    if (d[i].usage == 2) {
      g.raw_mouse_reg = reg;
      g.raw_mouse_window = reg ? d[i].hwnd : 0;
    } else if (d[i].usage == 6) {
      g.raw_kbd_reg = reg;
      g.raw_kbd_window = reg ? d[i].hwnd : 0;
    }
  }
  return true;
}

static const input_state::raw_rec* raw_record_locked(u64 handle) {
  if (!handle) return nullptr;
  const auto& raw = g.raw_ring[(handle - 1) % TRT_MSG_RING];
  return raw.handle == handle ? &raw : nullptr;
}

u64 trt_raw_target_window(u64 handle) {
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  const auto* raw = raw_record_locked(handle);
  return raw ? raw->window : 0;
}

bool trt_raw_copy(u64 handle, u8 out[48], u32* size) {
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  const auto* raw = raw_record_locked(handle);
  if (!raw) return false;
  *size = raw->size;
  memcpy(out, raw->blob, raw->size);
  return true;
}

u32 trt_raw_read_buffer(void* out, u32* size) {
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  const u32 capacity = out ? *size : 0;
  u32 written = 0, read = 0, next_size = 0;
  int kept = 0;
  bool full = false;
  for (int i = 0; i < g.ring_count; ++i) {
    const auto msg = g.ring[(g.ring_head + i) % TRT_MSG_RING];
    if (msg.message == 0x00FFu) {
      const auto* raw = raw_record_locked(static_cast<u64>(msg.lparam));
      if (!raw) continue;
      if (!out) {
        *size = raw->size;
        return 0;
      }
      if (!full && raw->size <= capacity - written) {
        memcpy(static_cast<u8*>(out) + written, raw->blob, raw->size);
        written += raw->size;
        ++read;
        continue;
      }
      full = true;
      if (!next_size) next_size = raw->size;
    }
    if (out) g.ring[(g.ring_head + kept++) % TRT_MSG_RING] = msg;
  }
  if (!out) {
    *size = 0;
    return 0;
  }
  g.ring_count = kept;
  if (!read && next_size) {
    *size = next_size;
    return (u32)-1;
  }
  return read;
}

// ---------------- message queue ----------------
unsigned long g_msg_dispatched_count = 0;  // STAGE 7 pump-dispatch counter
unsigned trt_msg_dispatched(void);         // (definition below, in trt.h ABI)

bool trt_msg_post(u32 message, u64 wparam, i64 lparam) {
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  if (message == 0x0200u && g.ring_count) {
    auto& last = g.ring[(g.ring_head + g.ring_count - 1) % TRT_MSG_RING];
    if (last.message == message) {
      last = {message, wparam, lparam, 0, 0};
      return true;
    }
  }
  if (g.ring_count >= TRT_MSG_RING) {
    static int drop_logged = 0;
    if (drop_logged < 5) {
      ++drop_logged;
      char line[128];
      std::snprintf(line, sizeof(line),
                    "TRACE input: RING FULL drop msg=0x%x wp=%llu",
                    message, (unsigned long long)wparam);
      trt_trace(line);
    }
    return false;
  }
  const int tail = (g.ring_head + g.ring_count) % TRT_MSG_RING;
  g.ring[tail] = trt_msg{message, wparam, lparam, 0, 0};
  ++g.ring_count;
  if (message == 0x0100u || message == 0x0101u || message == 0x0010u ||
      message == 0x0012u) {
    static int post_logged = 0;
    if (post_logged < 30) {
      ++post_logged;
      char line[128];
      std::snprintf(line, sizeof(line),
                    "TRACE input: post msg=0x%x wp=%llu ring=%d",
                    message, (unsigned long long)wparam, g.ring_count);
      trt_trace(line);
    }
  }
  return true;
}

bool trt_msg_peek_range(trt_msg* out, u32 minimum, u32 maximum, bool remove) {
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  for (int i = 0; i < g.ring_count; ++i) {
    const int index = (g.ring_head + i) % TRT_MSG_RING;
    const auto& msg = g.ring[index];
    if ((minimum || maximum) && (msg.message < minimum || msg.message > maximum))
      continue;
    if (out) *out = msg;
    if (remove) {
      if (i == 0) {
        g.ring_head = (g.ring_head + 1) % TRT_MSG_RING;
      } else {
        for (int j = i; j + 1 < g.ring_count; ++j)
          g.ring[(g.ring_head + j) % TRT_MSG_RING] =
              g.ring[(g.ring_head + j + 1) % TRT_MSG_RING];
      }
      --g.ring_count;
      ++g_msg_dispatched_count;
    }
    return true;
  }
  return false;
}

bool trt_msg_peek(trt_msg* out, bool remove) {
  return trt_msg_peek_range(out, 0, 0, remove);
}

bool trt_msg_pending() {
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  return g.ring_count > 0;
}

unsigned trt_msg_count(void) {
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  return (unsigned)g.ring_count;
}

unsigned trt_msg_dispatched(void) {
  std::lock_guard<std::recursive_mutex> lk(g_input_mu);
  return (unsigned)g_msg_dispatched_count;
}

} // extern "C"
