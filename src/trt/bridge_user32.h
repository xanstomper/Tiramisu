// tiramisu/src/trt/bridge_user32.h
// user32 for the guest (bridge_user32.cpp).
#ifndef TIRAMISU_BRIDGE_USER32_H
#define TIRAMISU_BRIDGE_USER32_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register user32.dll as BUILTIN.
int trt_bridge_register_user32(void);

// Window records remain private to bridge_user32. Return a copy of mutable
// data to adapters, with the lookup and access under the same lock.
i64 trt_user32_window_proc(u64 hwnd, const i64* replacement);
int trt_user32_window_title(u64 hwnd, u16* text, int capacity);

// ---- DC records (shared with bridge_gdi) ----
// GetDC hands out record handles; ReleaseDC frees them. bridge_gdi keys
// its per-DC state on these SAME records so SelectObject(hdc) from a
// user32 GetDC works and a fake DC fails visibly.
void* trt_user32_dc_alloc(void);
int   trt_user32_dc_release(void* hdc);
int   trt_user32_dc_is_valid(void* hdc);
void* trt_user32_dc_selected(void* hdc);
void  trt_user32_dc_set_selected(void* hdc, void* hobj);

// Per-DC GDI text state. It lives in the DC record (not in a side table) so a
// released and reused DC starts from the real GDI defaults instead of the
// previous window's mode: OPAQUE background, black text on white,
// TA_LEFT|TA_TOP. bridge_gdi is the only reader/writer.
typedef struct trt_dc_text_state {
  int bk_mode;           // 1 = TRANSPARENT, 2 = OPAQUE
  unsigned text_color;   // COLORREF 0x00BBGGRR
  unsigned bk_color;     // COLORREF
  unsigned text_align;   // TA_* flags
} trt_dc_text_state;
int trt_user32_dc_get_text_state(void* hdc, trt_dc_text_state* out);
int trt_user32_dc_set_text_state(void* hdc, const trt_dc_text_state* in);

// ---- Icon object table (shared with bridge_shell ExtractIconEx*) ----
void* trt_user32_icon_alloc(int w, int h);
int   trt_user32_icon_free(void* hicon);
int   trt_user32_icon_size(void* hicon, int* w_out, int* h_out);

#ifdef __cplusplus
}
#endif

#endif
