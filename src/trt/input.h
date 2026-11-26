// tiramisu/src/trt/input.h
// Input provider: the host feeds it (CoreWindow keyboard/pointer + XInput),
// guests read it through the user32 bridge. Same process, one consumer
// thread (the guest runs on the host loop), so a lock only guards handoff.
#ifndef TIRAMISU_INPUT_H
#define TIRAMISU_INPUT_H

#include "pe.h"

#define TRT_MAX_VK 256

// message ring for the guest pump (user32 GetMessage/PeekMessage)
typedef struct trt_msg {
  u32 message;   // WM_*
  u64 wparam;
  i64 lparam;
  u32 pointer_flags;
  u32 pointer_change;
} trt_msg;

#define TRT_MSG_RING 512

#ifdef __cplusplus
extern "C" {
#endif

// ---- host side ----
void trt_input_key_event(u16 vk, bool down);          // CoreWindow KeyDown/Up
void trt_input_key_event2(u16 vk, bool down, u16 scan, u8 extended,
                          u8 repeat);                 // with scan code
void trt_input_char_event(u32 ch);                     // for WM_CHAR
void trt_input_mouse_move(int x, int y);               // absolute; deltas accumulate
void trt_input_pointer_move(int x, int y, bool suppress_delta);  // UWP PointerMoved
void trt_input_set_cursor_pos(int x, int y);
void trt_input_set_clip_rect(bool enable, i32 left, i32 top, i32 right, i32 bottom);
bool trt_input_get_clip_rect(i32* left, i32* top, i32* right, i32* bottom);
void trt_input_mouse_delta_raw(i32 dx, i32 dy);        // REAL mouse deltas (MouseDevice)
void trt_input_mouse_button(int btn /*0=L 1=R 2=M 3=X1 4=X2*/, bool down);
void trt_input_wheel(i32 delta);
void trt_input_pad_state(i16 lx, i16 ly, i16 rx, i16 ry,
                         u16 buttons /*XINPUT_GAMEPAD_* bits*/,
                         u8 trigger_l, u8 trigger_r);
void trt_input_pad_state_n(u32 index /*0..3*/, bool connected,
                           i16 lx, i16 ly, i16 rx, i16 ry,
                           u16 buttons, u8 trigger_l, u8 trigger_r);
void trt_input_display_size(int w, int h);             // GetSystemMetrics
void trt_input_frame_begin();                          // clears per-frame bits
void trt_input_release_all(void);
bool trt_cursor_snapshot(int* x, int* y);
int trt_input_show_cursor(bool show);
void trt_input_set_cursor_shape(bool visible);
void trt_input_reset_cursor_mode(bool force_visible);
void trt_set_cursor_visible(bool v);                   // ShowCursor hook

// ---- guest side (user32 bridge) ----
bool  trt_key_down(u16 vk);
i16   trt_key_async(u16 vk);                           // GetAsyncKeyState value
i16   trt_key_sync(u16 vk);                            // GetKeyState value
int   trt_mouse_pos(int* x, int* y);
void  trt_mouse_delta(int* dx, int* dy);               // consumes accumulation
void  trt_mouse_delta_raw(int* dx, int* dy);           // last real-mouse deltas
bool  trt_mouse_button(int btn);
i32   trt_mouse_wheel();                               // consumes
bool  trt_pad_down(u16 xinput_bit);
float trt_pad_stick(int stick /*0=Lx 1=Ly 2=Rx 3=Ry*/);
i16   trt_pad_thumb(int stick /*0=Lx 1=Ly 2=Rx 3=Ry raw XInput range*/);
u8    trt_pad_trigger(int t /*0=L 1=R*/);
bool  trt_pad_connected(u32 index);
bool  trt_pad_down_n(u32 index, u16 xinput_bit);
i16   trt_pad_thumb_n(u32 index, int stick /*0=Lx 1=Ly 2=Rx 3=Ry*/);
u8    trt_pad_trigger_n(u32 index, int t /*0=L 1=R*/);
void  trt_pad_set_vibration(u32 index, u16 left_motor, u16 right_motor);
bool  trt_pad_get_vibration(u32 index, u16* left_motor, u16* right_motor);
int   trt_display_size(int* w, int* h);
u16   trt_key_scan(u16 vk);                            // last scan code per VK
void  trt_input_copy_dik_state(u8 out[256]);

// raw input (the layer most PC engines actually read)
bool trt_raw_register(const void* devices, u32 count); // RAWINPUTDEVICE[]
// hRawInput handles handed out through WM_INPUT lparams
bool trt_raw_copy(u64 handle, u8 out[48], u32* size);
u64 trt_raw_target_window(u64 handle);
u32 trt_raw_read_buffer(void* out, u32* size);

// ---- message queue ----
struct trt_mouse_pointer {
  int x, y;
  u32 frame, flags, time, change;
  i32 wheel;
  u32 key_states;
  u64 window;
};
void trt_input_enable_mouse_pointer(bool enable);
bool trt_input_mouse_pointer_enabled(void);
void trt_input_accept_pointer(const trt_msg* message, u64 window, u32 time);
bool trt_input_get_mouse_pointer(u32 id, trt_mouse_pointer* out);

bool trt_msg_post(u32 message, u64 wparam, i64 lparam);
bool trt_msg_peek(trt_msg* out, bool remove);
bool trt_msg_peek_range(trt_msg* out, u32 minimum, u32 maximum, bool remove);
bool trt_msg_pending();
unsigned trt_msg_count(void);  // diagnostic: current ring depth

#ifdef __cplusplus
}
#endif

#endif
