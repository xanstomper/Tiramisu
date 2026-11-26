// tiramisu/tests/test_input.cpp
// Offline proof for the input bridge: host feed -> user32 getters + pump.
#include "../src/trt/input.h"
#include "../src/host/cursor_policy.h"
#include "../src/host/cursor_art.h"
#include "../src/host/input_policy.h"
#include "../src/trt/pe.h"  // TRT_STDCALL: these are guest-ABI bridge fns —
                            // the local declaration MUST match the
                            // definition's convention or the arguments
                            // arrive in the wrong registers (STAGE 6)

#include <stdio.h>
#include <string.h>
#include <stdint.h>

extern "C" {
u16 TRT_STDCALL trt_RegisterClassExW(const void*);
void* TRT_STDCALL trt_CreateWindowExW(u32, const wchar_t*, const wchar_t*, u32,
                                     int, int, int, int, void*, void*, void*, void*);
void* TRT_STDCALL trt_SetFocus(void*);
void* TRT_STDCALL trt_GetFocus(void);
int TRT_STDCALL trt_ShowWindow(void*, int);
int TRT_STDCALL trt_PostMessageW(void*, u32, u64, i64);
int TRT_STDCALL trt_RegisterRawInputDevices(const void*, u32, u32);
u32 TRT_STDCALL trt_GetRawInputData(void*, u32, void*, u32*, u32);
u32 TRT_STDCALL trt_GetRawInputBuffer(void*, u32*, u32);
int TRT_STDCALL trt_EnableMouseInPointer(int);
int TRT_STDCALL trt_IsMouseInPointerEnabled(void);
int TRT_STDCALL trt_GetPointerInfo(u32, void*);
u32 TRT_STDCALL trt_MapVirtualKeyW(u32 code, u32 type);
int TRT_STDCALL trt_ToUnicode(u32 vk, u32 scan, const u8* ks, u16* buf,
                              int cap, u32 flags);
i16 TRT_STDCALL trt_VkKeyScanW(u16 ch);
void TRT_STDCALL trt_PostQuitMessage(i32 code);
int TRT_STDCALL trt_GetMessageW(void* out, void* hwnd, u32 min, u32 max);
int TRT_STDCALL trt_PeekMessageW(void* out, void* hwnd, u32 min, u32 max,
                                 u32 remove);
int TRT_STDCALL trt_ClipCursor(const void* lpRect);
int TRT_STDCALL trt_GetClipCursor(void* lpRect);
int TRT_STDCALL trt_SetCursorPos(int x, int y);
int TRT_STDCALL trt_GetSystemMetrics(int idx);
u32 TRT_STDCALL trt_GetMessagePos(void);
u32 TRT_STDCALL trt_XInputGetState(u32 user_index, void* state_out);
u32 TRT_STDCALL trt_XInputSetState(u32 user_index, const void* vibration);
}

extern "C" {
int trt_bridge_register_user32(void);
void trt_bridge_boot(const void* be);
}

int failures = 0;
#define CHECK(cond, msg)                              \
  do {                                                \
    if (cond) printf("  PASS %s\n", msg);             \
    else { printf("  FAIL %s\n", msg); ++failures; }  \
  } while (0)

struct focus_notification { u64 window; u32 message; u64 other; i64 reserved; };
static focus_notification focus_events[8];
static unsigned focus_event_count = 0;
static u64 created_window[8]{};
static unsigned created_window_count = 0;
static u32 rejected_window_message = 0;


static i64 TRT_STDCALL focus_window_proc(u64 window, u32 message, u64 other, i64 reserved) {
  if (message == 0x81u && rejected_window_message == message) return 0;
  if (message == 1) {
    if (rejected_window_message == message) return -1;
    if (created_window_count < 8) created_window[created_window_count++] = window;
  }
  if ((message == 7 || message == 8) && focus_event_count < 8)
    focus_events[focus_event_count++] = {window, message, other, reserved};
  return message == 0x81;
}

int main() {
  setvbuf(stdout, NULL, _IONBF, 0);
  printf("tiramisu offline input bridge test\n");
  trt_bridge_boot(nullptr);

  // keyboard: feed W down/up, read async/sync; drain its queue, then test A
  trt_input_key_event('W', true);
  CHECK(trt_key_down('W'), "W down");
  CHECK(trt_key_async('W') < 0, "GetAsyncKeyState negative while down");
  u8 ks[256];
  (void)ks;
  trt_input_key_event('W', false);
  CHECK(!trt_key_down('W'), "W released");
  CHECK(trt_key_async('W') == 1, "GetAsyncKeyState edge bit after release frame");
  {  // drain W's keydown/keyup from the queue
    trt_msg t{};
    while (trt_msg_peek(&t, true)) {}
  }

  // mouse: absolute moves -> position + consumed deltas
  trt_input_mouse_move(100, 100);
  trt_input_mouse_move(112, 95);
  int x = 0, y = 0;
  trt_mouse_pos(&x, &y);
  CHECK(x == 112 && y == 95, "mouse position");
  int dx = 0, dy = 0;
  trt_mouse_delta(&dx, &dy);
  CHECK(dx == 12 && dy == -5, "mouse deltas accumulate + consume");
  trt_mouse_delta(&dx, &dy);
  CHECK(dx == 0 && dy == 0, "deltas empty after consume");

  // Multiple Xbox input providers can report the same physical edge.
  trt_msg queued{};
  while (trt_msg_peek(&queued, true)) {}
  CHECK(trt_input_mouse_button(0, true) && trt_mouse_button(0), "left button down");
  CHECK(!trt_input_mouse_button(0, true), "second provider's press is ignored");
  CHECK(trt_input_mouse_button(0, false) && !trt_mouse_button(0), "left button up");
  CHECK(!trt_input_mouse_button(0, false), "second provider's release is ignored");
  CHECK(trt_msg_peek(&queued, true) && queued.message == 0x0201 &&
            trt_msg_peek(&queued, true) && queued.message == 0x0202 &&
            !trt_msg_peek(&queued, true),
        "overlapping physical providers generate one click");

  // pad
  trt_input_pad_state(32767, 0, 0, -32768, 0x1000 /*RT*/, 0, 255);
  CHECK(trt_pad_down(0x1000), "pad RT bit");
  CHECK(trt_pad_stick(0) > 0.9f, "LX normalized high");
  CHECK(trt_pad_stick(3) < -0.9f, "RY normalized low");
  CHECK(trt_pad_trigger(1) == 255, "right trigger");

  // message queue: drain all (mouse/pad noise), then a clean A down/up pair
  { trt_msg t{}; while (trt_msg_peek(&t, true)) {} }
  trt_input_key_event('A', true);
  trt_input_key_event('A', false);
  CHECK(trt_msg_pending(), "queue pending");
  trt_msg m{};
  CHECK(trt_msg_peek(&m, true) && m.message == 0x0100 && m.wparam == 'A',
        "WM_KEYDOWN A");
  CHECK(trt_msg_peek(&m, true) && m.message == 0x0101 && m.wparam == 'A',
        "WM_KEYUP A");
  CHECK(!trt_msg_pending(), "queue drained");

  // ---- game-grade surface ----
  // scan codes ride WM_KEYDOWN lParam (games read lParam>>16)
  {
    trt_msg t{};
    while (trt_msg_peek(&t, true)) {}
    trt_input_key_event2(0x11 /*VK_W? no, VK 'W'=0x57*/, true, 0x11, 0, 0);
    CHECK(trt_msg_peek(&t, true) && t.message == 0x0100 &&
              (((u32)t.lparam) >> 16) == 0x11,
          "scan code in KEYDOWN lParam");
  }
  // WM_CHAR from char events
  {
    trt_msg t{};
    while (trt_msg_peek(&t, true)) {}
    trt_input_char_event('h');
    CHECK(trt_msg_peek(&t, true) && t.message == 0x0102 && t.wparam == 'h',
          "WM_CHAR posted");
  }
  // Raw Input: register mouse+keyboard, raw deltas synthesize WM_INPUT with a
  // handle that GetRawInputData decodes back to the deltas
  {
    struct rid { u16 page, usage; u32 flags; u64 hwnd; };
    rid devs[2] = {{1, 2, 0, 0}, {1, 6, 0, 0}};  // mouse + keyboard
    CHECK(trt_RegisterRawInputDevices(devs, 2, 16) == 1,
          "RegisterRawInputDevices");
    { trt_msg t{}; while (trt_msg_peek(&t, true)) {} }
    trt_input_mouse_delta_raw(9, -7);
    trt_msg t{};
    CHECK(trt_msg_peek(&t, true) && t.message == 0x00FF, "WM_INPUT posted");
    u8 buf[64];
    u32 sz = sizeof(buf);
    const u32 got =
        trt_GetRawInputData((void*)(uintptr_t)t.lparam, 0x10000003, buf, &sz, 24);
    CHECK(got == 48, "GetRawInputData returns RAWMOUSE size");
    int rdx = 0, rdy = 0;
    memcpy(&rdx, buf + 36, 4);
    memcpy(&rdy, buf + 40, 4);
    CHECK(rdx == 9 && rdy == -7, "RAWMOUSE lLastX/lLastY round-trip");
    // size query form
    u32 q = 0;
    CHECK(trt_GetRawInputData((void*)(uintptr_t)t.lparam, 0x10000003, nullptr,
                              &q, 24) == 0 && q == 48,
          "GetRawInputData size query writes the required size");
    // stale handle -> -1 (0xFFFFFFFF), no crash
    CHECK(trt_GetRawInputData((void*)(uintptr_t)0xdeadbeef, 0x10000003, buf,
                              &sz, 24) == 0xFFFFFFFFu,
          "stale hRawInput -> -1");
  }

  // Unity drains one WM_INPUT and then asks for the remaining raw messages.
  {
    trt_msg t{};
    while (trt_msg_peek(&t, true)) {}
    trt_input_mouse_button(0, true);
    trt_input_key_event2('Q', true, 0x10, 0, 0);
    trt_input_mouse_button(0, false);

    u32 bytes = 0;
    CHECK(trt_GetRawInputBuffer(nullptr, &bytes, 24) == 0 && bytes == 48,
          "raw buffer size query leaves the first record queued");
    alignas(u64) u8 packet[136]{};
    bytes = 8;
    CHECK(trt_GetRawInputBuffer(packet, &bytes, 24) == 0xFFFFFFFFu && bytes == 48,
          "short raw buffer leaves the first record queued");
    bytes = sizeof(packet);
    CHECK(trt_GetRawInputBuffer(packet, &bytes, 24) == 3,
          "raw buffer returns mouse, keyboard and mouse in order");
    u16 flags = 0;
    memcpy(&flags, packet + 28, sizeof(flags));
    CHECK(flags == 1, "buffered mouse down flags");
    memcpy(&flags, packet + 48 + 24, sizeof(flags));
    CHECK(flags == 0x10, "buffered key keeps scan code");
    memcpy(&flags, packet + 88 + 28, sizeof(flags));
    CHECK(flags == 2, "buffered mouse up flags");
    CHECK(trt_msg_peek_range(&t, 0x0100, 0x0101, true) &&
              t.message == 0x0100 && t.wparam == 'Q' && (t.lparam & 0xFFFF) == 1,
          "filtered key read skips mouse messages and keeps repeat count");
    CHECK(trt_msg_peek(&t, true) && t.message == 0x0201 &&
              trt_msg_peek(&t, true) && t.message == 0x0202 && !trt_msg_pending(),
          "buffered raw read preserves standard mouse messages");

    trt_input_release_all();
    CHECK(!trt_key_down('Q'), "focus loss clears held key state");
    trt_input_reset_cursor_mode(false);
    CHECK(trt_input_show_cursor(false) == -1 &&
              !trt_cursor_snapshot(&x, &y) &&
              trt_input_show_cursor(true) == 0 &&
              trt_cursor_snapshot(&x, &y),
          "ShowCursor counter hides and restores the cursor");
    trt_input_reset_cursor_mode(true);
    CHECK(trt_input_show_cursor(false) == -1 && trt_cursor_snapshot(&x, &y),
          "forced console pointer stays visible when the guest hides its cursor");
    trt_input_reset_cursor_mode(false);
  }

  // Win32 pointer messages carry the button state at the time of the event.
  {
    trt_msg msg{};
    while (trt_msg_peek(&msg, true)) {}
    CHECK(trt_EnableMouseInPointer(1) && trt_IsMouseInPointerEnabled(),
          "EnableMouseInPointer requests pointer messages");
    trt_input_mouse_move(640, 400);
    trt_input_mouse_button(0, true);
    trt_input_mouse_button(0, false);
    CHECK(trt_msg_peek_range(&msg, 0x0246, 0x0246, true),
          "WM_POINTERDOWN is queued");
    trt_input_accept_pointer(&msg, 0x1234, 17);
    alignas(u64) u8 info[96]{};
    CHECK(trt_GetPointerInfo(1, info), "GetPointerInfo returns mouse event");
    u32 type, flags, change;
    int px, py;
    memcpy(&type, info, 4);
    memcpy(&flags, info + 12, 4);
    memcpy(&px, info + 32, 4);
    memcpy(&py, info + 36, 4);
    memcpy(&change, info + 88, 4);
    CHECK(type == 4 && (flags & 0x10010u) == 0x10010u &&
              px == 640 && py == 400 && change == 1,
          "pointer down snapshots position and pressed button after release");
    CHECK(trt_msg_peek_range(&msg, 0x0247, 0x0247, true),
          "WM_POINTERUP is queued");
    trt_input_accept_pointer(&msg, 0x1234, 18);
    trt_mouse_pointer release{};
    CHECK(trt_input_get_mouse_pointer(1, &release) &&
              (release.flags & 0x40010u) == 0x40000u && release.change == 2,
          "pointer up snapshots released button");
    while (trt_msg_peek(&msg, true)) {}
    trt_input_mouse_button(0, true);
    trt_input_mouse_button(1, true);
    trt_input_mouse_move(641, 401);
    CHECK(trt_msg_peek_range(&msg, 0x0245, 0x0245, true) && msg.pointer_change == 3,
          "motion never overwrites a second button transition");
    trt_input_mouse_button(1, false);
    trt_input_mouse_button(0, false);
    CHECK(trt_EnableMouseInPointer(0) && !trt_IsMouseInPointerEnabled(),
          "disabling pointer messages restores legacy mouse messages");
  }

  // RawInput keyboard layout (MakeCode +24, Flags +26, VKey +30) & RIDEV_REMOVE
  {
    struct rid { u16 page, usage; u32 flags; u64 hwnd; };
    rid r = {1, 6, 0, 0};
    CHECK(trt_RegisterRawInputDevices(&r, 1, sizeof(r)) == 1,
          "RegisterRawInputDevices(keyboard)");
    { trt_msg t{}; while (trt_msg_peek(&t, true)) {} }

    trt_input_key_event2('W', true, 0x11, 0, 0);
    trt_msg tk1{}, ti1{};
    CHECK(trt_msg_peek(&tk1, true) && tk1.message == 0x0100u &&
          trt_msg_peek(&ti1, true) && ti1.message == 0x00FFu,
          "WM_KEYDOWN + WM_INPUT posted for raw keyboard");
    u8 kbuf[64] = {};
    u32 ksz = sizeof(kbuf);
    const u32 kgot =
        trt_GetRawInputData((void*)(uintptr_t)ti1.lparam, 0x10000003, kbuf, &ksz, 24);
    u64 kdev = 0;
    u16 mk = 0, kfl = 0, kres = 0, kvkey = 0;
    u32 kmsg = 0;
    memcpy(&kdev, kbuf + 8, 8);
    memcpy(&mk, kbuf + 24, 2);
    memcpy(&kfl, kbuf + 26, 2);
    memcpy(&kres, kbuf + 28, 2);
    memcpy(&kvkey, kbuf + 30, 2);
    memcpy(&kmsg, kbuf + 32, 4);
    CHECK(kgot == 40 && kdev == 0x00060002ULL && mk == 0x11 && kfl == 0 &&
              kres == 0 && kvkey == 'W' && kmsg == 0x0100u,
          "RAWKEYBOARD MakeCode/+26 Flags/+30 VKey keydown layout");

    trt_input_key_event2('W', false, 0x11, 1, 0);
    trt_msg tk2{}, ti2{};
    trt_msg_peek(&tk2, true);
    CHECK(trt_msg_peek(&ti2, true) && ti2.message == 0x00FFu,
          "WM_INPUT posted for raw keyboard keyup");
    ksz = sizeof(kbuf);
    trt_GetRawInputData((void*)(uintptr_t)ti2.lparam, 0x10000003, kbuf, &ksz, 24);
    memcpy(&kfl, kbuf + 26, 2);
    memcpy(&kvkey, kbuf + 30, 2);
    memcpy(&kmsg, kbuf + 32, 4);
    CHECK(kfl == 3 /*RI_KEY_BREAK|RI_KEY_E0*/ && kvkey == 'W' && kmsg == 0x0101u,
          "RAWKEYBOARD RI_KEY_BREAK + E0 + VKey(+30) keyup layout");

    rid rem = {1, 6, 1 /*RIDEV_REMOVE*/, 0};
    trt_RegisterRawInputDevices(&rem, 1, sizeof(rem));
    trt_input_key_event('W', true);
    trt_msg t1{}, t2{};
    CHECK(trt_msg_peek(&t1, true) && t1.message == 0x0100u &&
              !trt_msg_peek(&t2, true),
          "RIDEV_REMOVE stops raw WM_INPUT generation");
  }

  // Shift/Ctrl/Alt L<->R sync
  {
    trt_input_key_event2(0xA0 /*VK_LSHIFT*/, true, 0x2A, 0, 0);
    CHECK(trt_key_down(0x10 /*VK_SHIFT*/), "LSHIFT sets generic VK_SHIFT");
    trt_input_key_event2(0xA0, false, 0x2A, 0, 0);
    CHECK(!trt_key_down(0x10), "LSHIFT release clears generic VK_SHIFT");
    trt_input_key_event2(0x10 /*VK_SHIFT*/, true, 0x2A, 0, 0);
    CHECK(trt_key_down(0xA0 /*VK_LSHIFT*/), "VK_SHIFT sets VK_LSHIFT");
    trt_input_key_event2(0x10, false, 0x2A, 0, 0);
    CHECK(!trt_key_down(0xA0), "VK_SHIFT release clears VK_LSHIFT");
  }

  // MapVirtualKeyW (0/1/2/3/4), ToUnicode (Shift), VkKeyScanW
  {
    CHECK(trt_MapVirtualKeyW('W', 0) == 0x11u, "MapVirtualKeyW(0) VK->scan");
    CHECK(trt_MapVirtualKeyW(0x11, 1) == 'W', "MapVirtualKeyW(1) scan->VK");
    CHECK(trt_MapVirtualKeyW('W', 2) == 'W' &&
              trt_MapVirtualKeyW(0xBA /*VK_OEM_1*/, 2) == ';',
          "MapVirtualKeyW(2) VK->char uppercase & OEM");
    CHECK(trt_MapVirtualKeyW(0x36, 1) == 0x10u /*VK_SHIFT*/ &&
              trt_MapVirtualKeyW(0x36, 3) == 0xA1u /*VK_RSHIFT*/ &&
              trt_MapVirtualKeyW(0xE01D, 3) == 0xA3u /*VK_RCONTROL*/,
          "MapVirtualKeyW(1/3) generic vs extended L/R modifier");

    u8 ks[256] = {};
    u16 ch[8] = {};
    CHECK(trt_ToUnicode('A', 0, ks, ch, 8, 0) == 1 && ch[0] == 'a',
          "ToUnicode unshifted alpha -> lowercase");
    ks[0x10] = 0x80;  // VK_SHIFT down
    CHECK(trt_ToUnicode('A', 0, ks, ch, 8, 0) == 1 && ch[0] == 'A',
          "ToUnicode shifted alpha -> uppercase");
    CHECK(trt_ToUnicode('1', 0, ks, ch, 8, 0) == 1 && ch[0] == '!',
          "ToUnicode shifted digit -> symbol");
    CHECK(trt_ToUnicode(0xDB /*VK_OEM_4*/, 0, ks, ch, 8, 0) == 1 && ch[0] == '{',
          "ToUnicode shifted OEM -> '{'");
    CHECK(trt_VkKeyScanW('!') == (i16)('1' | 0x0100) &&
              trt_VkKeyScanW('a') == 'A' &&
              trt_VkKeyScanW('A') == (i16)('A' | 0x0100),
          "VkKeyScanW returns VK + shift state");
  }

  // ClipCursor + GetClipCursor
  {
    i32 rc[4] = {100, 100, 200, 200};
    i32 out[4] = {};
    CHECK(trt_ClipCursor(rc) == 1 && trt_GetClipCursor(out) == 1 &&
              out[0] == 100 && out[1] == 100 && out[2] == 200 && out[3] == 200,
          "ClipCursor + GetClipCursor round-trip");
    trt_input_mouse_move(500, 500);
    int px = 0, py = 0;
    trt_mouse_pos(&px, &py);
    CHECK(px == 199 && py == 199, "mouse_move clamps within ClipCursor rect");
    trt_ClipCursor(nullptr);
  }

  // PostQuitMessage -> WM_QUIT with hwnd == 0 and GetMessageW == 0
  {
    trt_msg drain{};
    while (trt_msg_peek(&drain, true)) {}
    trt_PostQuitMessage(42);
    struct {
      u64 hwnd;
      u32 msg;
      u32 pad;
      u64 wp;
      i64 lp;
      u32 time;
      i32 pt[2];
    } gm = {};
    gm.hwnd = 0x12345678ULL;
    const int grc = trt_GetMessageW(&gm, nullptr, 0, 0);
    CHECK(grc == 0 && gm.hwnd == 0 && gm.msg == 0x0012u && gm.wp == 42u,
          "PostQuitMessage -> GetMessageW returns 0 with WM_QUIT hwnd==0");
    CHECK(trt_PeekMessageW(&gm, nullptr, 0, 0, 1) == 0,
          "WM_QUIT consumed cleanly after GetMessageW");
  }

  // Mouse X1/X2, MK_* wParam bits, raw-dedup PointerMoved, SetCursorPos, 4-pad XInput
  {
    struct rid_rem { u16 page, usage; u32 flags; u64 hwnd; } rem_m = {1, 2, 1, 0};
    trt_RegisterRawInputDevices(&rem_m, 1, sizeof(rem_m));
    trt_input_key_event(0x11, false);  // release held VK_CONTROL from earlier scan test
    trt_msg drain{};
    while (trt_msg_peek(&drain, true)) {}
    int dx = 0, dy = 0;
    trt_mouse_delta(&dx, &dy);

    trt_input_mouse_button(3 /*XBUTTON1*/, true);
    CHECK(trt_mouse_button(3) && trt_key_async(0x05 /*VK_XBUTTON1*/) < 0,
          "XBUTTON1 updates mouse button state and VK_XBUTTON1 async state");
    trt_msg m1{};
    CHECK(trt_msg_peek(&m1, true) && m1.message == 0x020Bu /*WM_XBUTTONDOWN*/ &&
              m1.wparam == ((1ULL << 16) | 0x0020u),
          "WM_XBUTTONDOWN carries XBUTTON1<<16 | MK_XBUTTON1");

    trt_input_mouse_button(0 /*L*/, true);
    trt_input_mouse_move(120, 80);
    trt_msg m2{}, m3{};
    trt_msg_peek(&m2, true);
    CHECK(trt_msg_peek(&m3, true) && m3.message == 0x0200u /*WM_MOUSEMOVE*/ &&
              m3.wparam == 0x0021u /*MK_LBUTTON|MK_XBUTTON1*/,
          "WM_MOUSEMOVE populates live MK_* button flags in wParam");

    trt_input_mouse_button(4 /*XBUTTON2*/, true);
    CHECK(trt_mouse_button(4) && trt_key_async(0x06 /*VK_XBUTTON2*/) < 0,
          "XBUTTON2 updates mouse button state and VK_XBUTTON2 async state");
    trt_input_mouse_button(4, false);
    trt_input_mouse_button(3, false);
    trt_input_mouse_button(0, false);
    while (trt_msg_peek(&drain, true)) {}
    trt_mouse_delta(&dx, &dy);

    // UWP raw-mouse dedup: suppress_delta=true updates cursor pos without adding delta
    trt_input_pointer_move(210, 160, true);
    int mx = 0, my = 0;
    trt_mouse_pos(&mx, &my);
    trt_mouse_delta(&dx, &dy);
    CHECK(mx == 210 && my == 160 && dx == 0 && dy == 0,
          "pointer_move(suppress_delta=true) updates pos without double-counting delta");

    // SetCursorPos warps without adding fake jump into delta accumulator
    CHECK(trt_SetCursorPos(100, 100) == 1, "SetCursorPos returns TRUE");
    trt_mouse_pos(&mx, &my);
    trt_mouse_delta(&dx, &dy);
    CHECK(mx == 100 && my == 100 && dx == 0 && dy == 0,
          "SetCursorPos moves cursor without injecting fake delta jump");
    {  // a warp must notify the window, like Windows does
      bool saw_move = false;
      trt_msg wm{};
      while (trt_msg_peek(&wm, true)) {
        if (wm.message == 0x0200u /*WM_MOUSEMOVE*/) saw_move = true;
      }
      CHECK(saw_move, "SetCursorPos posts WM_MOUSEMOVE for guests that wait on it");
    }
    // games gate mouse creation on these metrics
    {
      struct { int idx, want; } sm[] = {
          {19, 1}, {43, 5}, {75, 1}, {13, 32}, {14, 32}, {80, 1}, {23, 0}};
      bool ok = true;
      for (const auto& e : sm) ok = ok && trt_GetSystemMetrics(e.idx) == e.want;
      CHECK(ok, "GetSystemMetrics reports mouse, buttons, wheel and cursor metrics");
    }

    // Hidden/locked cursor refuses OS pointer snapback when raw mouse owns input
    trt_set_cursor_visible(false);
    trt_input_pointer_move(600, 600, true);
    trt_mouse_pos(&mx, &my);
    CHECK(mx == 100 && my == 100,
          "suppress_delta pointer move does not snap back cursor while hidden");
    trt_set_cursor_visible(true);

    // Multi-controller XInput (slots 0..3) + rumble round-trip
    trt_input_pad_state_n(0, true, 1111, -1111, 0, 0, 0x1000u, 11, 22);
    trt_input_pad_state_n(1, true, 2222, -2222, 333, -333, 0x2000u, 33, 44);
    trt_input_pad_state_n(2, false, 0, 0, 0, 0, 0, 0, 0);
    trt_input_pad_state_n(3, true, -3333, 3333, 0, 0, 0x4000u, 55, 66);

    struct {
      u32 pkt;
      u16 btns;
      u8 tl, tr;
      i16 lx, ly, rx, ry;
    } xs{};
    const u32 r0 = trt_XInputGetState(0, &xs);
    const i16 lx0 = xs.lx;
    const u32 r1 = trt_XInputGetState(1, &xs);
    const u16 b1 = xs.btns;
    const u32 r2 = trt_XInputGetState(2, &xs);
    const u32 r3 = trt_XInputGetState(3, &xs);
    CHECK(r0 == 0 && lx0 == 1111 && r1 == 0 && b1 == 0x2000u &&
              r2 == 1167u /*ERROR_DEVICE_NOT_CONNECTED*/ &&
              r3 == 0 && xs.lx == -3333,
          "XInputGetState supports 4 independent slots and disconnected detection");

    u16 vib[2] = {12345u, 54321u};
    u16 got_l = 0, got_r = 0;
    CHECK(trt_XInputSetState(1, vib) == 0 &&
              trt_pad_get_vibration(1, &got_l, &got_r) &&
              got_l == 12345u && got_r == 54321u &&
              trt_XInputSetState(2, vib) == 1167u,
          "XInputSetState stores per-slot vibration and checks connection");
  }

  // A pad, mouse button or key held when the console suspends the app must
  // not look held after resume: the host re-polls the device on the way back.
  {
    trt_input_pad_state(32767, 32767, 0, 0, 0x1000, 255, 255);
    trt_input_mouse_button(0, true);
    trt_input_key_event('W', true);
    CHECK(trt_pad_down(0x1000) && trt_mouse_button(0) && trt_key_down('W'),
          "input held before suspend is visible");
    trt_input_release_all();
    CHECK(!trt_pad_down(0x1000) && trt_pad_thumb(0) == 0 &&
              trt_pad_trigger(0) == 0,
          "suspend releases every pad button, stick and trigger");
    CHECK(!trt_mouse_button(0) && !trt_key_down('W'),
          "suspend releases mouse buttons and keys");
  }

  {
    using longan::cursor_owner;
    longan::cursor_context cursor{true, false, false, false, true, false, true, false, false};
    CHECK(longan::choose_cursor(cursor) == cursor_owner::launcher,
          "hardware input keeps the dashboard arrow after the pointer stops");
    cursor.guest_running = true;
    CHECK(longan::choose_cursor(cursor) == cursor_owner::system,
          "a guest without a composited cursor leaves the shell arrow available");
    cursor.guest_can_draw = true;
    CHECK(longan::choose_cursor(cursor) == cursor_owner::guest,
          "a live guest overlay draws the Tiramisu arrow");
    cursor.quick_menu = true;
    CHECK(longan::choose_cursor(cursor) == cursor_owner::launcher,
          "opening the quick menu returns cursor ownership to the launcher");
    cursor.controller_active = true;
    CHECK(longan::choose_cursor(cursor) == cursor_owner::hidden,
          "controller-only UI can hide the pointer");
    cursor.virtual_pointer = true;
    CHECK(longan::choose_cursor(cursor) == cursor_owner::launcher,
          "virtual pointer remains visible when selected");
    cursor.focused = false;
    CHECK(longan::choose_cursor(cursor) == cursor_owner::system,
          "losing focus returns cursor ownership to the shell");
    CHECK(longan::pointer_event_recent(1000, 850, 300) &&
              !longan::pointer_event_recent(1000, 1001, 300) &&
              !longan::pointer_event_recent(1000, 700, 300),
          "arbitration handles idle and newly published event times");
  }
  {
    using namespace longan;
    const auto auto_mode = resolve_input_mode("auto", "");
    CHECK(route_gamepads(auto_mode, true, false) == guest_pad_route::physical &&
              route_gamepads(auto_mode, false, false) == guest_pad_route::physical,
          "Auto exposes physical pad slots with or without a controller connected");
    CHECK(resolve_input_mode("controller", "auto") == guest_input_mode::controller &&
              route_gamepads(guest_input_mode::controller, false, false) == guest_pad_route::keyboard &&
              route_gamepads(guest_input_mode::controller, true, false) == guest_pad_route::physical,
          "one global controller preference provides a keyboard fallback without merging devices");
    CHECK(route_gamepads(auto_mode, true, true) == guest_pad_route::disabled &&
              route_gamepads(resolve_input_mode("controller", "kbm"), true, false) ==
                  guest_pad_route::disabled,
          "explicit keyboard and mouse modes do not double-report controller input");
    CHECK(cursor_height(720) == 12 && cursor_height(1080) == 18 && cursor_height(2160) == 32,
          "cursor renderers share compact output-pixel sizes");
  }
  {
    struct {
      u32 size, style;
      void* proc;
      int class_extra, window_extra;
      void* instance;
      void* icon;
      void* cursor;
      void* background;
      const wchar_t* menu;
      const wchar_t* name;
      void* small_icon;
    } cls{};
    cls.size = sizeof(cls);
    cls.name = L"TiramisuFocusTest";
    cls.proc = reinterpret_cast<void*>(&focus_window_proc);
    CHECK(trt_RegisterClassExW(&cls) != 0, "registered a window procedure for focus tests");
    constexpr u32 visible = 0x10000000u;
    constexpr u32 child_style = 0x40000000u;
    auto* first = trt_CreateWindowExW(0, cls.name, nullptr, visible, 0, 0, 640, 480,
                                      nullptr, nullptr, nullptr, nullptr);
    auto* helper = trt_CreateWindowExW(0, cls.name, nullptr, 0, 0, 0, 320, 240,
                                       nullptr, nullptr, nullptr, nullptr);
    CHECK(helper && trt_GetFocus() == first,
          "a hidden helper window leaves the visible window focused");
    auto* child = trt_CreateWindowExW(0, cls.name, nullptr, visible | child_style,
                                      0, 0, 64, 64, first, nullptr, nullptr, nullptr);
    CHECK(child && trt_GetFocus() == first,
          "creating a visible child does not take keyboard focus");
    CHECK(trt_ShowWindow(helper, 8 /*SW_SHOWNA*/) == 0 && trt_GetFocus() == first,
          "showing a helper without activation keeps the existing focus");
    auto* second = trt_CreateWindowExW(0, cls.name, nullptr, 0, 0, 0, 640, 480,
                                       nullptr, nullptr, nullptr, nullptr);
    CHECK(trt_ShowWindow(second, 5 /*SW_SHOW*/) == 0 && trt_GetFocus() == second,
          "showing a top-level window with activation focuses it");
    focus_event_count = 0;
    CHECK(first && second && first != second, "created distinct guest windows");
    struct {
      u64 window;
      u32 message;
      u32 padding;
      u64 wp;
      i64 lp;
      u32 time;
      i32 point[2];
    } queued{};
    CHECK(created_window_count == 4 &&
              created_window[0] == reinterpret_cast<u64>(first) &&
              created_window[3] == reinterpret_cast<u64>(second),
          "CreateWindowEx sends WM_CREATE synchronously even without creation data");
    CHECK(!trt_PeekMessageW(&queued, nullptr, 1, 1, 1),
          "registered windows receive no duplicate queued WM_CREATE");
    rejected_window_message = 0x81u;
    CHECK(!trt_CreateWindowExW(0, cls.name, nullptr, visible, 0, 0, 640, 480,
                               nullptr, nullptr, nullptr, nullptr) && trt_GetFocus() == second,
          "WM_NCCREATE can reject a window without taking focus");
    rejected_window_message = 1;
    CHECK(!trt_CreateWindowExW(0, cls.name, nullptr, visible, 0, 0, 640, 480,
                               nullptr, nullptr, nullptr, nullptr) && trt_GetFocus() == second,
          "WM_CREATE returning minus one aborts window creation");
    rejected_window_message = 0;
    CHECK(trt_PostMessageW(first, 0x0401u, 42, 0) &&
              trt_PeekMessageW(&queued, nullptr, 0x0401u, 0x0401u, 1) &&
              queued.window == reinterpret_cast<u64>(first) && queued.wp == 42,
          "PostMessage delivers to the specified window despite a different focus");
    CHECK(trt_PostMessageW(nullptr, 0x0402u, 0, 0) &&
              trt_PeekMessageW(&queued, nullptr, 0x0402u, 0x0402u, 1) &&
              queued.window == 0,
          "PostMessage with a null window stays a thread message");
    CHECK(trt_SetFocus(first) == second && trt_GetFocus() == first,
          "SetFocus returns the previous window and updates the new focus");
    CHECK(focus_event_count == 2 &&
              focus_events[0].window == reinterpret_cast<u64>(second) &&
              focus_events[0].message == 8 &&
              focus_events[0].other == reinterpret_cast<u64>(first) &&
              focus_events[0].reserved == 0 &&
              focus_events[1].window == reinterpret_cast<u64>(first) &&
              focus_events[1].message == 7 &&
              focus_events[1].other == reinterpret_cast<u64>(second) &&
              focus_events[1].reserved == 0,
          "focus transfer synchronously notifies both windows with the counterpart HWND");
    CHECK(trt_SetFocus(first) == first && focus_event_count == 2,
          "retaining the same focus sends no duplicate notifications");
    CHECK(trt_SetFocus(reinterpret_cast<void*>(0xDEAD)) == nullptr &&
              trt_GetFocus() == first && focus_event_count == 2,
          "invalid focus target leaves the current window unchanged");
    CHECK(trt_SetFocus(nullptr) == first && trt_GetFocus() == nullptr &&
              focus_event_count == 3 &&
              focus_events[2].window == reinterpret_cast<u64>(first) &&
              focus_events[2].message == 8 && focus_events[2].other == 0,
          "clearing focus notifies the window that lost it");
  }

  printf(failures ? "\nRESULT: %d FAILURES\n" : "\nRESULT: ALL PASS\n",
         failures);
  return failures ? 1 : 0;
}
