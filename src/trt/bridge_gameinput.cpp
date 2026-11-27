// tiramisu/src/trt/bridge_gameinput.cpp
// GameInput (gameinput.dll) COM bridge for modern GDK and Steam PC titles.
// Maps IGameInput -> IGameInputDevice / IGameInputReading -> input.h (XInput
// 4-slot gamepad + keyboard + mouse + rumble feedback).
#include "bridge_gameinput.h"
#include "input.h"
#include "trt.h"
#include "thunk.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string.h>

namespace {

[[maybe_unused]] constexpr i32 S_OK_ = 0;
[[maybe_unused]] constexpr i32 E_POINTER_ = (i32)0x80004003u;
[[maybe_unused]] constexpr i32 E_NOINTERFACE_ = (i32)0x80004002u;
[[maybe_unused]] constexpr i32 E_INVALIDARG_ = (i32)0x80070057u;
[[maybe_unused]] constexpr i32 E_NOTIMPL_ = (i32)0x80004001u;
[[maybe_unused]] constexpr i32 GAMEINPUT_E_READING_NOT_FOUND_ = (i32)0x838A0001u;

// GameInputKind enum bits
[[maybe_unused]] constexpr u32 k_kind_unknown = 0x00000000u;
[[maybe_unused]] constexpr u32 k_kind_raw = 0x00000001u;
[[maybe_unused]] constexpr u32 k_kind_controller_axis = 0x00000002u;
[[maybe_unused]] constexpr u32 k_kind_controller_button = 0x00000004u;
[[maybe_unused]] constexpr u32 k_kind_controller_switch = 0x00000008u;
[[maybe_unused]] constexpr u32 k_kind_controller = 0x0000000Eu;
[[maybe_unused]] constexpr u32 k_kind_keyboard = 0x00000010u;
[[maybe_unused]] constexpr u32 k_kind_mouse = 0x00000020u;
[[maybe_unused]] constexpr u32 k_kind_gamepad = 0x00010000u;
[[maybe_unused]] constexpr u32 k_kind_any = 0x0FFFFFFFu;

u64 monotonic_us() {
  using namespace std::chrono;
  return (u64)duration_cast<microseconds>(steady_clock::now().time_since_epoch())
      .count();
}

struct gi_gamepad_state {
  u32 buttons;
  float leftTrigger;
  float rightTrigger;
  float leftThumbstickX;
  float leftThumbstickY;
  float rightThumbstickX;
  float rightThumbstickY;
};

struct gi_mouse_state {
  u32 buttons;
  u32 _pad;
  i64 positionX;
  i64 positionY;
  i64 wheelX;
  i64 wheelY;
};

struct gi_key_state {
  u32 scanCode;
  u32 codePoint;
  u8 virtualKey;
  u8 isDeadKey;
  u8 _pad[2];
};

struct gi_rumble_params {
  float lowFrequency;
  float highFrequency;
  float leftTrigger;
  float rightTrigger;
};

struct gi_device_obj {
  const void** vtbl;
  std::atomic<u32> ref;
  u32 pad_index;
  u32 supported_kinds;
};

struct gi_reading_obj {
  const void** vtbl;
  std::atomic<u32> ref;
  u32 kind;
  u64 seq;
  u64 ts;
  gi_device_obj* dev;
};

struct gi_engine_obj {
  const void** vtbl;
  std::atomic<u32> ref;
};

extern const void* g_gi_device_vtbl[18];
extern const void* g_gi_reading_vtbl[29];
extern const void* g_gi_engine_vtbl[21];

gi_device_obj g_pad0_device = {g_gi_device_vtbl, {1}, 0,
                               k_kind_gamepad | k_kind_controller};

// Map XInput button mask -> GameInputGamepadButtons
u32 xinput_to_gi_buttons(u32 idx) {
  u32 out = 0;
  // XINPUT -> GI bit map
  if (trt_pad_down_n(idx, 0x0010u)) out |= 0x0001u;  // Start -> Menu
  if (trt_pad_down_n(idx, 0x0020u)) out |= 0x0002u;  // Back  -> View
  if (trt_pad_down_n(idx, 0x1000u)) out |= 0x0004u;  // A
  if (trt_pad_down_n(idx, 0x2000u)) out |= 0x0008u;  // B
  if (trt_pad_down_n(idx, 0x4000u)) out |= 0x0010u;  // X
  if (trt_pad_down_n(idx, 0x8000u)) out |= 0x0020u;  // Y
  if (trt_pad_down_n(idx, 0x0001u)) out |= 0x0040u;  // DPad Up
  if (trt_pad_down_n(idx, 0x0002u)) out |= 0x0080u;  // DPad Down
  if (trt_pad_down_n(idx, 0x0004u)) out |= 0x0100u;  // DPad Left
  if (trt_pad_down_n(idx, 0x0008u)) out |= 0x0200u;  // DPad Right
  if (trt_pad_down_n(idx, 0x0100u)) out |= 0x0400u;  // LB
  if (trt_pad_down_n(idx, 0x0200u)) out |= 0x0800u;  // RB
  if (trt_pad_down_n(idx, 0x0040u)) out |= 0x1000u;  // LS
  if (trt_pad_down_n(idx, 0x0080u)) out |= 0x2000u;  // RS
  return out;
}

float norm_i16(i16 v) {
  return (v < 0) ? ((float)v / 32768.0f) : ((float)v / 32767.0f);
}

// ---- IGameInputDevice ----
i32 TRT_GUEST_ABI gid_QI(void* s, const u8*, void** out) {
  if (!out) return E_POINTER_;
  *out = s;
  ((gi_device_obj*)s)->ref.fetch_add(1, std::memory_order_relaxed);
  return S_OK_;
}
u32 TRT_GUEST_ABI gid_AddRef(void* s) {
  return ((gi_device_obj*)s)->ref.fetch_add(1, std::memory_order_relaxed) + 1u;
}
u32 TRT_GUEST_ABI gid_Release(void* s) {
  auto* d = (gi_device_obj*)s;
  const u32 r = d->ref.fetch_sub(1, std::memory_order_acq_rel);
  return r > 1u ? (r - 1u) : 1u;  // static singleton
}

struct gi_device_info_min {
  u32 infoSize;
  u16 vendorId;
  u16 productId;
  u16 revisionNumber;
  u8 interfaceNumber;
  u8 collectionNumber;
  u16 usagePage;
  u16 usage;
  u32 deviceId[8];
  u32 deviceRootId[8];
  u32 deviceFamily;
  u32 capabilities;
  u32 supportedInput;
  u32 supportedRumbleMotors;
  u32 inputReportCount;
  u32 outputReportCount;
  u32 featureReportCount;
  u32 controllerAxisCount;
  u32 controllerButtonCount;
  u32 controllerSwitchCount;
  u32 keyboardSize;
  u32 mouseButtonCount;
  u32 touchPointCount;
  u32 sensorCount;
  u8 _rest[64];
};
static gi_device_info_min g_pad0_info = []() {
  gi_device_info_min info{};
  info.infoSize = sizeof(gi_device_info_min);
  info.vendorId = 0x045Eu;   // Microsoft
  info.productId = 0x02FFu;  // Xbox One / Series Controller
  info.deviceFamily = 2u;    // GameInputFamilyXboxOne
  info.supportedInput = k_kind_gamepad | k_kind_controller;
  info.supportedRumbleMotors = 0x0Fu;
  info.controllerAxisCount = 6u;
  info.controllerButtonCount = 14u;
  info.controllerSwitchCount = 1u;
  return info;
}();

const void* TRT_GUEST_ABI gid_GetDeviceInfo(void*) { return &g_pad0_info; }
u32 TRT_GUEST_ABI gid_GetDeviceStatus(void* s) {
  auto* d = (gi_device_obj*)s;
  return trt_pad_connected(d->pad_index) ? 0x00000003u /*Connected|AnyInput*/ : 0u;
}
void TRT_GUEST_ABI gid_GetBatteryState(void*, void* state) {
  if (state) memset(state, 0, 16);
}
i32 TRT_GUEST_ABI gid_CreateForceFeedbackEffect(void*, u32, const void*, void** out) {
  if (out) *out = nullptr;
  return E_NOTIMPL_;
}
u8 TRT_GUEST_ABI gid_IsForceFeedbackMotorPoweredOn(void*, u32) { return 0; }
void TRT_GUEST_ABI gid_SetForceFeedbackMotorGain(void*, u32, float) {}
i32 TRT_GUEST_ABI gid_SetHapticMotorState(void*, u32, const void*) {
  return E_NOTIMPL_;
}
void TRT_GUEST_ABI gid_SetRumbleState(void* s, const gi_rumble_params* p) {
  auto* d = (gi_device_obj*)s;
  if (!p) {
    trt_pad_set_vibration(d->pad_index, 0, 0);
    return;
  }
  auto clamp01 = [](float f) -> u16 {
    if (f <= 0.0f) return 0;
    if (f >= 1.0f) return 65535u;
    return (u16)(f * 65535.0f);
  };
  trt_pad_set_vibration(d->pad_index, clamp01(p->lowFrequency),
                        clamp01(p->highFrequency));
}
void TRT_GUEST_ABI gid_SetInputSynchronizationState(void*, u8) {}
void TRT_GUEST_ABI gid_SendInputSynchronizationHint(void*) {}
void TRT_GUEST_ABI gid_PowerOff(void*) {}
i32 TRT_GUEST_ABI gid_CreateRawDeviceReport(void*, u32, u32, void** out) {
  if (out) *out = nullptr;
  return E_NOTIMPL_;
}
i32 TRT_GUEST_ABI gid_GetRawDeviceFeature(void*, u32, void**) { return E_NOTIMPL_; }
i32 TRT_GUEST_ABI gid_SetRawDeviceFeature(void*, void*) { return E_NOTIMPL_; }
i32 TRT_GUEST_ABI gid_SendRawDeviceOutput(void*, void*) { return E_NOTIMPL_; }

const void* g_gi_device_vtbl[18] = {
    (const void*)&gid_QI,
    (const void*)&gid_AddRef,
    (const void*)&gid_Release,
    (const void*)&gid_GetDeviceInfo,
    (const void*)&gid_GetDeviceStatus,
    (const void*)&gid_GetBatteryState,
    (const void*)&gid_CreateForceFeedbackEffect,
    (const void*)&gid_IsForceFeedbackMotorPoweredOn,
    (const void*)&gid_SetForceFeedbackMotorGain,
    (const void*)&gid_SetHapticMotorState,
    (const void*)&gid_SetRumbleState,
    (const void*)&gid_SetInputSynchronizationState,
    (const void*)&gid_SendInputSynchronizationHint,
    (const void*)&gid_PowerOff,
    (const void*)&gid_CreateRawDeviceReport,
    (const void*)&gid_GetRawDeviceFeature,
    (const void*)&gid_SetRawDeviceFeature,
    (const void*)&gid_SendRawDeviceOutput,
};

// ---- IGameInputReading ----
i32 TRT_GUEST_ABI gir_QI(void* s, const u8*, void** out) {
  if (!out) return E_POINTER_;
  *out = s;
  ((gi_reading_obj*)s)->ref.fetch_add(1, std::memory_order_relaxed);
  return S_OK_;
}
u32 TRT_GUEST_ABI gir_AddRef(void* s) {
  return ((gi_reading_obj*)s)->ref.fetch_add(1, std::memory_order_relaxed) + 1u;
}
u32 TRT_GUEST_ABI gir_Release(void* s) {
  auto* r = (gi_reading_obj*)s;
  const u32 c = r->ref.fetch_sub(1, std::memory_order_acq_rel) - 1u;
  if (c == 0) delete r;
  return c;
}
u32 TRT_GUEST_ABI gir_GetInputKind(void* s) { return ((gi_reading_obj*)s)->kind; }
u64 TRT_GUEST_ABI gir_GetSequenceNumber(void* s, u32) {
  return ((gi_reading_obj*)s)->seq;
}
u64 TRT_GUEST_ABI gir_GetTimestamp(void* s) { return ((gi_reading_obj*)s)->ts; }
void TRT_GUEST_ABI gir_GetDevice(void* s, void** dev_out) {
  if (!dev_out) return;
  auto* r = (gi_reading_obj*)s;
  if (r->dev) {
    gid_AddRef(r->dev);
    *dev_out = r->dev;
  } else {
    *dev_out = nullptr;
  }
}
u8 TRT_GUEST_ABI gir_GetRawReport(void*, void** out) {
  if (out) *out = nullptr;
  return 0;
}
u32 TRT_GUEST_ABI gir_GetControllerAxisCount(void*) { return 6u; }
u32 TRT_GUEST_ABI gir_GetControllerAxisState(void*, u32 count, float* state) {
  if (!state || count == 0) return 0;
  float axes[6] = {
      norm_i16(trt_pad_thumb_n(0, 0)),
      norm_i16(trt_pad_thumb_n(0, 1)),
      norm_i16(trt_pad_thumb_n(0, 2)),
      norm_i16(trt_pad_thumb_n(0, 3)),
      (float)trt_pad_trigger_n(0, 0) / 255.0f,
      (float)trt_pad_trigger_n(0, 1) / 255.0f,
  };
  u32 n = count < 6u ? count : 6u;
  memcpy(state, axes, n * sizeof(float));
  return n;
}
u32 TRT_GUEST_ABI gir_GetControllerButtonCount(void*) { return 14u; }
u32 TRT_GUEST_ABI gir_GetControllerButtonState(void*, u32 count, u8* state) {
  if (!state || count == 0) return 0;
  const u32 mask = xinput_to_gi_buttons(0);
  u32 n = count < 14u ? count : 14u;
  for (u32 i = 0; i < n; ++i) state[i] = (mask & (1u << i)) ? 1u : 0u;
  return n;
}
u32 TRT_GUEST_ABI gir_GetControllerSwitchCount(void*) { return 1u; }
u32 TRT_GUEST_ABI gir_GetControllerSwitchState(void*, u32 count, u32* state) {
  if (!state || count == 0) return 0;
  const bool u = trt_pad_down_n(0, 0x0001u);
  const bool d = trt_pad_down_n(0, 0x0002u);
  const bool l = trt_pad_down_n(0, 0x0004u);
  const bool r = trt_pad_down_n(0, 0x0008u);
  u32 sw = 0;  // GameInputSwitchCenter
  if (u && r) sw = 2; else if (r && d) sw = 4;
  else if (d && l) sw = 6; else if (l && u) sw = 8;
  else if (u) sw = 1; else if (r) sw = 3;
  else if (d) sw = 5; else if (l) sw = 7;
  state[0] = sw;
  return 1u;
}
u32 TRT_GUEST_ABI gir_GetKeyCount(void*) {
  u32 n = 0;
  for (int vk = 1; vk < 255; ++vk) {
    if (trt_key_down((u16)vk)) ++n;
  }
  return n;
}
u32 TRT_GUEST_ABI gir_GetKeyState(void*, u32 count, gi_key_state* states) {
  if (!states || count == 0) return 0;
  u32 written = 0;
  for (int vk = 1; vk < 255 && written < count; ++vk) {
    if (trt_key_down((u16)vk)) {
      states[written].scanCode = trt_key_scan((u16)vk);
      states[written].codePoint = (u32)vk;
      states[written].virtualKey = (u8)vk;
      states[written].isDeadKey = 0;
      ++written;
    }
  }
  return written;
}
u8 TRT_GUEST_ABI gir_GetMouseState(void*, gi_mouse_state* st) {
  if (!st) return 0;
  memset(st, 0, sizeof(*st));
  int mx = 0, my = 0;
  trt_mouse_pos(&mx, &my);
  st->positionX = mx;
  st->positionY = my;
  if (trt_mouse_button(0)) st->buttons |= 0x01u;
  if (trt_mouse_button(1)) st->buttons |= 0x02u;
  if (trt_mouse_button(2)) st->buttons |= 0x04u;
  if (trt_mouse_button(3)) st->buttons |= 0x08u;
  if (trt_mouse_button(4)) st->buttons |= 0x10u;
  st->wheelY = trt_mouse_wheel();
  return 1;
}
u32 TRT_GUEST_ABI gir_GetTouchCount(void*) { return 0; }
u32 TRT_GUEST_ABI gir_GetTouchState(void*, u32, void*) { return 0; }
u8 TRT_GUEST_ABI gir_GetMotionState(void*, void* st) {
  if (st) memset(st, 0, 48);
  return 0;
}
u8 TRT_GUEST_ABI gir_GetArcadeStickState(void*, void*) { return 0; }
u8 TRT_GUEST_ABI gir_GetFlightStickState(void*, void*) { return 0; }
u8 TRT_GUEST_ABI gir_GetGamepadState(void* s, gi_gamepad_state* st) {
  if (!st) return 0;
  auto* r = (gi_reading_obj*)s;
  const u32 idx = r->dev ? r->dev->pad_index : 0;
  if (!trt_pad_connected(idx)) {
    memset(st, 0, sizeof(*st));
    return 0;
  }
  st->buttons = xinput_to_gi_buttons(idx);
  st->leftTrigger = (float)trt_pad_trigger_n(idx, 0) / 255.0f;
  st->rightTrigger = (float)trt_pad_trigger_n(idx, 1) / 255.0f;
  st->leftThumbstickX = norm_i16(trt_pad_thumb_n(idx, 0));
  st->leftThumbstickY = norm_i16(trt_pad_thumb_n(idx, 1));
  st->rightThumbstickX = norm_i16(trt_pad_thumb_n(idx, 2));
  st->rightThumbstickY = norm_i16(trt_pad_thumb_n(idx, 3));
  return 1;
}
u8 TRT_GUEST_ABI gir_GetRacingWheelState(void*, void*) { return 0; }
u8 TRT_GUEST_ABI gir_GetUiNavigationState(void*, void*) { return 0; }

const void* g_gi_reading_vtbl[29] = {
    (const void*)&gir_QI,
    (const void*)&gir_AddRef,
    (const void*)&gir_Release,
    (const void*)&gir_GetInputKind,
    (const void*)&gir_GetSequenceNumber,
    (const void*)&gir_GetTimestamp,
    (const void*)&gir_GetDevice,
    (const void*)&gir_GetRawReport,
    (const void*)&gir_GetControllerAxisCount,
    (const void*)&gir_GetControllerAxisState,
    (const void*)&gir_GetControllerButtonCount,
    (const void*)&gir_GetControllerButtonState,
    (const void*)&gir_GetControllerSwitchCount,
    (const void*)&gir_GetControllerSwitchState,
    (const void*)&gir_GetKeyCount,
    (const void*)&gir_GetKeyState,
    (const void*)&gir_GetMouseState,
    (const void*)&gir_GetTouchCount,
    (const void*)&gir_GetTouchState,
    (const void*)&gir_GetMotionState,
    (const void*)&gir_GetArcadeStickState,
    (const void*)&gir_GetFlightStickState,
    (const void*)&gir_GetGamepadState,
    (const void*)&gir_GetRacingWheelState,
    (const void*)&gir_GetUiNavigationState,
    (const void*)&gir_GetTouchCount,
    (const void*)&gir_GetTouchState,
    (const void*)&gir_GetMotionState,
    (const void*)&gir_GetArcadeStickState,
};

// ---- IGameInput ----
std::atomic<u64> g_gi_seq{1};
std::atomic<u64> g_gi_tok{1};

i32 TRT_GUEST_ABI gi_QI(void* s, const u8*, void** out) {
  if (!out) return E_POINTER_;
  *out = s;
  ((gi_engine_obj*)s)->ref.fetch_add(1, std::memory_order_relaxed);
  return S_OK_;
}
u32 TRT_GUEST_ABI gi_AddRef(void* s) {
  return ((gi_engine_obj*)s)->ref.fetch_add(1, std::memory_order_relaxed) + 1u;
}
u32 TRT_GUEST_ABI gi_Release(void* s) {
  auto* e = (gi_engine_obj*)s;
  const u32 c = e->ref.fetch_sub(1, std::memory_order_acq_rel) - 1u;
  if (c == 0) delete e;
  return c;
}
u64 TRT_GUEST_ABI gi_GetCurrentTimestamp(void*) { return monotonic_us(); }

i32 TRT_GUEST_ABI gi_GetCurrentReading(void*, u32 kind, void* dev, void** out) {
  if (!out) return E_POINTER_;
  *out = nullptr;
  const bool want_pad = (kind & (k_kind_gamepad | k_kind_controller)) != 0;
  const bool want_kb_mouse = (kind & (k_kind_keyboard | k_kind_mouse)) != 0;
  if (want_pad && !want_kb_mouse && !trt_pad_connected(0)) {
    return GAMEINPUT_E_READING_NOT_FOUND_;
  }
  auto* r = new gi_reading_obj();
  r->vtbl = g_gi_reading_vtbl;
  r->ref.store(1, std::memory_order_relaxed);
  u32 got_kind = kind & (k_kind_keyboard | k_kind_mouse);
  if (trt_pad_connected(0) && (kind & (k_kind_gamepad | k_kind_controller))) {
    got_kind |= (kind & (k_kind_gamepad | k_kind_controller));
  }
  r->kind = got_kind ? got_kind : kind;
  r->seq = g_gi_seq.fetch_add(1, std::memory_order_relaxed);
  r->ts = monotonic_us();
  r->dev = dev ? (gi_device_obj*)dev : &g_pad0_device;
  *out = r;
  return S_OK_;
}
i32 TRT_GUEST_ABI gi_GetNextReading(void*, void*, u32, void*, void** out) {
  if (out) *out = nullptr;
  return GAMEINPUT_E_READING_NOT_FOUND_;
}
i32 TRT_GUEST_ABI gi_GetPreviousReading(void*, void*, u32, void*, void** out) {
  if (out) *out = nullptr;
  return GAMEINPUT_E_READING_NOT_FOUND_;
}
i32 TRT_GUEST_ABI gi_GetTemporalReading(void*, u64, void*, void** out) {
  if (out) *out = nullptr;
  return GAMEINPUT_E_READING_NOT_FOUND_;
}
i32 TRT_GUEST_ABI gi_RegisterReadingCallback(void*, void*, u32, float, void*,
                                              void*, u64* token) {
  if (token) *token = g_gi_tok.fetch_add(1, std::memory_order_relaxed);
  return S_OK_;
}
i32 TRT_GUEST_ABI gi_RegisterDeviceCallback(void*, void*, u32, u32, u32 flags,
                                             void* ctx, void* cb, u64* token) {
  const u64 tok = g_gi_tok.fetch_add(1, std::memory_order_relaxed);
  if (token) *token = tok;
  // GameInputBlockingEnumeration (0x1): synchronously enumerate connected pad 0
  if (cb && ((flags & 0x1u) || trt_pad_connected(0))) {
    using dev_cb_t = void(TRT_GUEST_ABI*)(u64, void*, void*, u64, u32, u32);
    if (trt_pad_connected(0)) {
      ((dev_cb_t)cb)(tok, ctx, &g_pad0_device, monotonic_us(), 0x3u, 0x3u);
    }
  }
  return S_OK_;
}
i32 TRT_GUEST_ABI gi_RegisterSystemButtonCallback(void*, void*, u32, void*,
                                                   void*, u64* token) {
  if (token) *token = g_gi_tok.fetch_add(1, std::memory_order_relaxed);
  return S_OK_;
}
i32 TRT_GUEST_ABI gi_RegisterKeyboardLayoutCallback(void*, void*, void*, void*,
                                                     u64* token) {
  if (token) *token = g_gi_tok.fetch_add(1, std::memory_order_relaxed);
  return S_OK_;
}
void TRT_GUEST_ABI gi_StopCallback(void*, u64) {}
u8 TRT_GUEST_ABI gi_UnregisterCallback(void*, u64, u64) { return 1; }
i32 TRT_GUEST_ABI gi_CreateAggregateDevice(void*, u32, void** out) {
  if (out) *out = nullptr;
  return E_NOTIMPL_;
}
i32 TRT_GUEST_ABI gi_FindDeviceFromId(void*, const void*, void** out) {
  if (!out) return E_POINTER_;
  gid_AddRef(&g_pad0_device);
  *out = &g_pad0_device;
  return S_OK_;
}
i32 TRT_GUEST_ABI gi_FindDeviceFromObject(void*, void*, void** out) {
  if (out) *out = nullptr;
  return E_NOTIMPL_;
}
i32 TRT_GUEST_ABI gi_FindDeviceFromPlatformHandle(void*, void*, void** out) {
  if (out) *out = nullptr;
  return E_NOTIMPL_;
}
i32 TRT_GUEST_ABI gi_FindDeviceFromPlatformString(void*, const u16*, void** out) {
  if (out) *out = nullptr;
  return E_NOTIMPL_;
}
i32 TRT_GUEST_ABI gi_EnableOemDeviceSupport(void*, u16, u16, u8, u8) {
  return S_OK_;
}
void TRT_GUEST_ABI gi_SetFocusPolicy(void*, u32) {}

const void* g_gi_engine_vtbl[21] = {
    (const void*)&gi_QI,
    (const void*)&gi_AddRef,
    (const void*)&gi_Release,
    (const void*)&gi_GetCurrentTimestamp,
    (const void*)&gi_GetCurrentReading,
    (const void*)&gi_GetNextReading,
    (const void*)&gi_GetPreviousReading,
    (const void*)&gi_GetTemporalReading,
    (const void*)&gi_RegisterReadingCallback,
    (const void*)&gi_RegisterDeviceCallback,
    (const void*)&gi_RegisterSystemButtonCallback,
    (const void*)&gi_RegisterKeyboardLayoutCallback,
    (const void*)&gi_StopCallback,
    (const void*)&gi_UnregisterCallback,
    (const void*)&gi_CreateAggregateDevice,
    (const void*)&gi_FindDeviceFromId,
    (const void*)&gi_FindDeviceFromObject,
    (const void*)&gi_FindDeviceFromPlatformHandle,
    (const void*)&gi_FindDeviceFromPlatformString,
    (const void*)&gi_EnableOemDeviceSupport,
    (const void*)&gi_SetFocusPolicy,
};

i32 TRT_GUEST_ABI gi_GameInputCreate(void** out) {
  if (!out) return E_POINTER_;
  auto* e = new gi_engine_obj();
  e->vtbl = g_gi_engine_vtbl;
  e->ref.store(1, std::memory_order_relaxed);
  *out = e;
  trt_core_log("gameinput: GameInputCreate -> BUILTIN IGameInput (XInput+KBM)");
  return S_OK_;
}

}  // namespace

extern "C" int trt_bridge_register_gameinput(void) {
  static const trt_named_fn k_fns[] = {
      {"GameInputCreate", (void*)&gi_GameInputCreate},
  };
  trt_module_register("gameinput.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  return 0;
}
