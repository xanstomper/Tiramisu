// tiramisu/src/trt/bridge_xinput.cpp
// XInput bridge. See bridge_xinput.h.
#include "bridge_xinput.h"
#include "input.h"
#include "thunk.h"
#include "trt.h"

#include <atomic>
#include <stdio.h>
#include <string.h>

namespace xinputlog {
static void line(const char* s) { trt_core_log(s); }
}  // namespace xinputlog

namespace {

constexpr u32 k_err_success = 0u;
constexpr u32 k_err_empty = 4306u;  // ERROR_EMPTY
constexpr u32 k_err_device_not_connected = 1167u;

struct xinput_gamepad {
  u16 wButtons;
  u8 bLeftTrigger;
  u8 bRightTrigger;
  i16 sThumbLX;
  i16 sThumbLY;
  i16 sThumbRX;
  i16 sThumbRY;
};

static_assert(sizeof(trt_pad_sample) == 16, "XINPUT_STATE size mismatch");

struct xinput_vibration {
  u16 wLeftMotorSpeed;
  u16 wRightMotorSpeed;
};

struct xinput_capabilities {
  u8 Type;
  u8 SubType;
  u16 Flags;
  xinput_gamepad Gamepad;
  xinput_vibration Vibration;
};
static_assert(sizeof(xinput_capabilities) == 20,
              "XINPUT_CAPABILITIES size mismatch");

struct xinput_battery_info {
  u8 BatteryType;
  u8 BatteryLevel;
};

struct xinput_keystroke {
  u16 VirtualKey;
  u16 Unicode;
  u16 Flags;
  u8 UserIndex;
  u8 HidCode;
};

std::atomic<bool> g_enabled{true};

}  // namespace

extern "C" {

u32 TRT_GUEST_ABI trt_XInputGetState(u32 user_index, void* state_out) {
  gap_trace_record("XInputGetState");
  static std::atomic<unsigned> state_polls{0};
  const unsigned poll_no = state_polls.fetch_add(1, std::memory_order_relaxed);
  trt_pad_sample state{};
  if (!trt_pad_read_sample(user_index, &state)) {
    if (poll_no < 16) {
      char line[96];
      snprintf(line, sizeof(line),
               "xinput: GetState #%u user=%u disconnected",
               poll_no + 1, user_index);
      xinputlog::line(line);
    }
    return k_err_device_not_connected;
  }
  if (!state_out) return 87u;  // ERROR_INVALID_PARAMETER
  if (!g_enabled.load(std::memory_order_relaxed)) state = {};
  memcpy(state_out, &state, sizeof(state));
  static std::atomic<u16> last_buttons[4]{};
  const auto& pad = state.controls;
  const bool button_changed = last_buttons[user_index].exchange(
      pad.buttons, std::memory_order_relaxed) != pad.buttons;
  if (poll_no < 16 || button_changed) {
    char line[128];
    snprintf(line, sizeof(line),
             "xinput: GetState #%u user=%u buttons=%04x lx=%d ly=%d",
             poll_no + 1, user_index, pad.buttons,
             pad.lx, pad.ly);
    xinputlog::line(line);
  }
  return k_err_success;
}

u32 TRT_GUEST_ABI trt_XInputSetState(u32 user_index, const void* vibration) {
  if (!trt_pad_connected(user_index)) return k_err_device_not_connected;
  if (vibration) {
    const xinput_vibration* v = (const xinput_vibration*)vibration;
    trt_pad_set_vibration(user_index, v->wLeftMotorSpeed, v->wRightMotorSpeed);
  }
  return k_err_success;
}

u32 TRT_GUEST_ABI trt_XInputGetCapabilities(u32 user_index, u32 flags,
                                            void* caps_out) {
  (void)flags;
  if (!trt_pad_connected(user_index)) return k_err_device_not_connected;
  if (!caps_out) return 87u;
  xinput_capabilities* c = (xinput_capabilities*)caps_out;
  memset(c, 0, sizeof(*c));
  c->Type = 1;     // XINPUT_DEVTYPE_GAMEPAD
  c->SubType = 1;  // XINPUT_DEVSUBTYPE_GAMEPAD
  c->Flags = 0;
  c->Gamepad.wButtons = 0xF3FFu;
  c->Gamepad.bLeftTrigger = 0xFFu;
  c->Gamepad.bRightTrigger = 0xFFu;
  c->Gamepad.sThumbLX = -64;
  c->Gamepad.sThumbLY = -64;
  c->Gamepad.sThumbRX = -64;
  c->Gamepad.sThumbRY = -64;
  c->Vibration.wLeftMotorSpeed = 0xFFFFu;
  c->Vibration.wRightMotorSpeed = 0xFFFFu;
  return k_err_success;
}

void TRT_GUEST_ABI trt_XInputEnable(int enable) {
  g_enabled = (enable != 0);
  char buf[96];
  snprintf(buf, sizeof(buf), "xinput: XInputEnable(%d)", enable);
  xinputlog::line(buf);
}

u32 TRT_GUEST_ABI trt_XInputGetBatteryInformation(u32 user_index, u8 dev_type,
                                                  void* bat_out) {
  (void)dev_type;
  if (!trt_pad_connected(user_index)) return k_err_device_not_connected;
  if (!bat_out) return 87u;
  xinput_battery_info* b = (xinput_battery_info*)bat_out;
  b->BatteryType = 2;   // BATTERY_TYPE_ALKALINE
  b->BatteryLevel = 3;  // BATTERY_LEVEL_FULL
  return k_err_success;
}

u32 TRT_GUEST_ABI trt_XInputGetKeystroke(u32 user_index, u32 reserved,
                                         void* keystroke_out) {
  (void)reserved;
  if (user_index != 0xFFu && !trt_pad_connected(user_index)) {
    return k_err_device_not_connected;
  }
  if (keystroke_out) memset(keystroke_out, 0, sizeof(xinput_keystroke));
  return k_err_empty;
}

u32 TRT_GUEST_ABI trt_XInputGetAudioDeviceIds(u32 user_index, u16* render_id,
                                              u32* render_count,
                                              u16* capture_id,
                                              u32* capture_count) {
  if (!trt_pad_connected(user_index)) return k_err_device_not_connected;
  if (render_id && render_count && *render_count > 0) render_id[0] = 0;
  if (render_count) *render_count = 0;
  if (capture_id && capture_count && *capture_count > 0) capture_id[0] = 0;
  if (capture_count) *capture_count = 0;
  return k_err_success;
}

u32 TRT_GUEST_ABI trt_XInputGetDSoundAudioDeviceGuids(u32 user_index,
                                                      void* render_guid,
                                                      void* capture_guid) {
  if (!trt_pad_connected(user_index)) return k_err_device_not_connected;
  if (render_guid) memset(render_guid, 0, 16);
  if (capture_guid) memset(capture_guid, 0, 16);
  return k_err_success;
}

u32 TRT_GUEST_ABI trt_XInputWaitForGuideButton(u32 user_index, u32 flags, void* unreserved) {
  (void)flags;
  (void)unreserved;
  return trt_pad_connected(user_index) ? k_err_success : k_err_device_not_connected;
}

u32 TRT_GUEST_ABI trt_XInputCancelGuideButtonWait(u32 user_index) {
  return trt_pad_connected(user_index) ? k_err_success : k_err_device_not_connected;
}

u32 TRT_GUEST_ABI trt_XInputPowerOffController(u32 user_index) {
  return trt_pad_connected(user_index) ? k_err_success : k_err_device_not_connected;
}

int trt_bridge_register_xinput(void) {
  static const trt_named_fn k_fns[] = {
      {"XInputGetState", (void*)&trt_XInputGetState},
      {"XInputGetStateEx", (void*)&trt_XInputGetState},
      {"XInputSetState", (void*)&trt_XInputSetState},
      {"XInputGetCapabilities", (void*)&trt_XInputGetCapabilities},
      {"XInputEnable", (void*)&trt_XInputEnable},
      {"XInputGetBatteryInformation", (void*)&trt_XInputGetBatteryInformation},
      {"XInputGetKeystroke", (void*)&trt_XInputGetKeystroke},
      {"XInputGetAudioDeviceIds", (void*)&trt_XInputGetAudioDeviceIds},
      {"XInputGetDSoundAudioDeviceGuids",
       (void*)&trt_XInputGetDSoundAudioDeviceGuids},
      {"XInputWaitForGuideButton", (void*)&trt_XInputWaitForGuideButton},
      {"XInputCancelGuideButtonWait", (void*)&trt_XInputCancelGuideButtonWait},
      {"XInputPowerOffController", (void*)&trt_XInputPowerOffController},
      // Ordinals imported directly by UnityPlayer.dll (#2, #3) and SDL2 (#100..#103)
      {"#2", (void*)&trt_XInputGetState},
      {"#3", (void*)&trt_XInputSetState},
      {"#4", (void*)&trt_XInputGetCapabilities},
      {"#5", (void*)&trt_XInputEnable},
      {"#6", (void*)&trt_XInputGetAudioDeviceIds},
      {"#7", (void*)&trt_XInputGetBatteryInformation},
      {"#8", (void*)&trt_XInputGetKeystroke},
      {"#100", (void*)&trt_XInputGetState},
      {"#101", (void*)&trt_XInputWaitForGuideButton},
      {"#102", (void*)&trt_XInputCancelGuideButtonWait},
      {"#103", (void*)&trt_XInputPowerOffController},
  };
  const int n = (int)(sizeof(k_fns) / sizeof(k_fns[0]));
  trt_module_register("xinput1_4.dll", TRT_ROUTE_BUILTIN, k_fns, n);
  trt_module_register("xinput1_3.dll", TRT_ROUTE_BUILTIN, k_fns, n);
  trt_module_register("xinput1_2.dll", TRT_ROUTE_BUILTIN, k_fns, n);
  trt_module_register("xinput1_1.dll", TRT_ROUTE_BUILTIN, k_fns, n);
  trt_module_register("xinput9_1_0.dll", TRT_ROUTE_BUILTIN, k_fns, n);
  trt_module_register("xinputuap.dll", TRT_ROUTE_BUILTIN, k_fns, n);
  return 0;
}

}  // extern "C"
