// tiramisu/src/trt/bridge_dinput.cpp
// DirectInput 8 for the guest.
//
// Most PC games read the mouse and keyboard through DirectInput 8, not
// through Win32 messages. Without it a game boots and gets NO input at
// all - so this bridge is what makes the mouse actually move on screen.
//
// Two COM objects with the SDK's exact vtables:
//   IDirectInput8W          3 + 8  slots
//   IDirectInputDevice8W    3 + 31 slots
// The device hands out the classic states: DIMOUSESTATE (dx/dy/buttons)
// and the 256-byte keyboard state (indexed by DIK scan code).
#include "bridge_dinput.h"
#include "bridge_ole.h"
#include "thunk.h"
#include "input.h"

#include <stdio.h>
#include <atomic>
#include <stdlib.h>
#include <string.h>

// user32 vk<->scan source of truth (defined in input.cpp/bridge_user32.cpp)
extern "C" u32 TRT_STDCALL trt_MapVirtualKeyW(u32 code, u32 type);

namespace {
// named-line logging (fail-visible surface, same shape as the other bridges)
void di_log(const char* s) { trt_core_log(s); }
#define log_line(s_) di_log(s_)

namespace {

constexpr int E_NOTIMPL_ = 0x80004001;
constexpr int DI_OK_ = 0;

// the keyboard and mouse GUIDs (dinput.h)
// GUID_SysKeyboard {6F1D2B61-D5A0-11CF-BFC7-444553540000} (in-memory GUID)
const u8 k_guid_keyboard[16] = {0x61, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF,
                                0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54,
                                0x00, 0x00};
// GUID_SysMouse {6F1D2B60-D5A0-11CF-BFC7-444553540000} (in-memory GUID)
const u8 k_guid_mouse[16] = {0x60, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11,
                             0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00};
// Tiramisu Virtual Xbox Controller GUID
const u8 k_guid_gamepad[16] = {0x54, 0x49, 0x52, 0x41, 0x4D, 0x49, 0x53, 0x55,
                               0x50, 0x41, 0x44, 0x30, 0x00, 0x00, 0x00, 0x01};

enum device_kind { DEV_KEYBOARD = 1, DEV_MOUSE = 2, DEV_GAMEPAD = 3 };

struct dinput_device {
  const void** vtbl;
  u32 refcount;
  u32 kind;         // device_kind
  u32 acquired;
  u32 coop_level;
};

struct dinput_object {
  const void** vtbl;
  u32 refcount;
};

// DIMOUSESTATE: LONG lX, lY, lZ + BYTE rgbButtons[8] (0x80 = down)
struct mouse_state {
  i32 lX;
  i32 lY;
  i32 lZ;
  u8 rgbButtons[8];
};

// DIJOYSTATE (80 bytes) / DIJOYSTATE2 prefix (matches standard Win32 dinput.h layout)
struct joy_state {
  i32 lX;
  i32 lY;
  i32 lZ;
  i32 lRx;
  i32 lRy;
  i32 lRz;
  i32 rglSlider[2];
  u32 rgdwPOV[4];
  u8 rgbButtons[128];
  // DIJOYSTATE2 extra fields ignored unless buffer >= 272
  i32 lVX, lVY, lVZ, lVRx, lVRy, lVRz, rglVSlider[2];
  i32 lAX, lAY, lAZ, lARx, lARy, lARz, rglASlider[2];
  i32 lFX, lFY, lFZ, lFRx, lFRy, lFRz, rglFSlider[2];
};

// the objects and vtables are defined further down
extern dinput_device g_dev_keyboard;
extern dinput_device g_dev_mouse;
extern dinput_device g_dev_gamepad;
extern const void* g_di8_vtbl[11];
extern const void* g_didev_vtbl[34];


// the objects and the vtables are defined below

// ---- IUnknown (shared by both objects) ----
int TRT_GUEST_ABI di_QueryInterface(void* self, const u8* riid, void** out) {
  (void)riid;
  if (out) *out = self;
  return DI_OK_;
}
u32 TRT_GUEST_ABI di_AddRef(void* self) {
  u32* r = (u32*)self;   // refcount is the second field of both objects
  return ++r[1];
}
u32 TRT_GUEST_ABI di_Release(void* self) {
  u32* r = (u32*)self;
  return r[1] ? --r[1] : 0;
}

// ---- IDirectInput8 ----
int TRT_GUEST_ABI di_CreateDevice(void* self, const u8* guid, void** out_device,
                                  void* unk) {
  (void)self; (void)unk;
  if (!out_device) return E_NOTIMPL_;
  if (guid && memcmp(guid, k_guid_mouse, 16) == 0) {
    *out_device = &g_dev_mouse;
  } else if (guid && memcmp(guid, k_guid_keyboard, 16) == 0) {
    *out_device = &g_dev_keyboard;
  } else if (guid && memcmp(guid, k_guid_gamepad, 16) == 0) {
    *out_device = &g_dev_gamepad;
  } else {
    *out_device = &g_dev_mouse;
  }
  return DI_OK_;
}

static void fill_di_dev_inst(void* dst_raw, const u8* guid, u32 dev_type,
                             const char* name) {
  // DIDEVICEINSTANCEW (1080 bytes) or DIDEVICEINSTANCEA (580 bytes)
  u8* d = (u8*)dst_raw;
  u32 sz = *(u32*)d;
  if (sz < 40u) return;
  memset(d + 4, 0, sz - 4);
  memcpy(d + 4, guid, 16);
  memcpy(d + 20, guid, 16);
  memcpy(d + 36, &dev_type, 4);
  if (sz >= 1080u) {
    u16* inst = (u16*)(d + 40);
    u16* prod = (u16*)(d + 560);
    for (int i = 0; name[i] && i < 259; ++i) {
      inst[i] = (u16)(u8)name[i];
      prod[i] = (u16)(u8)name[i];
    }
  } else if (sz >= 580u) {
    char* inst = (char*)(d + 40);
    char* prod = (char*)(d + 300);
    for (int i = 0; name[i] && i < 259; ++i) {
      inst[i] = name[i];
      prod[i] = name[i];
    }
  }
}

int TRT_GUEST_ABI di_EnumDevices(void* self, u32 type, void* callback,
                                 void* context, u32 flags) {
  (void)self; (void)flags;
  if (!callback) return E_NOTIMPL_;
  using cb_t = int (TRT_GUEST_ABI*)(const void*, void*);
  u8 buf[1120];
  auto emit = [&](const u8* guid, u32 dev_type, const char* name) -> int {
    memset(buf, 0, sizeof(buf));
    u32 sz = 1080u;
    memcpy(buf, &sz, 4);
    fill_di_dev_inst(buf, guid, dev_type, name);
    return ((cb_t)callback)(buf, context);
  };
  // DI8DEVCLASS_ALL (0), DI8DEVCLASS_DEVICE (1), DI8DEVCLASS_POINTER (2),
  // DI8DEVCLASS_KEYBOARD (3), DI8DEVCLASS_GAMECTRL (4)
  if (type == 0 || type == 2) {
    if (emit(k_guid_mouse, 0x00000212u, "Tiramisu Mouse") == 0) return DI_OK_;
  }
  if (type == 0 || type == 3) {
    if (emit(k_guid_keyboard, 0x00000413u, "Tiramisu Keyboard") == 0) return DI_OK_;
  }
  if ((type == 0 || type == 4) && trt_pad_connected(0)) {
    emit(k_guid_gamepad, 0x00010215u, "Xbox Controller (Tiramisu)");
  }
  return DI_OK_;
}

int TRT_GUEST_ABI di_GetDeviceStatus(void* self, const u8* guid) {
  (void)self; (void)guid;
  return DI_OK_;  // DISCONNECTED == 0, so say "connected"
}
int TRT_GUEST_ABI di_RunControlPanel(void*, void*, u32) { return E_NOTIMPL_; }
int TRT_GUEST_ABI di_Initialize(void*, void*, u32) { return DI_OK_; }
int TRT_GUEST_ABI di_FindDevice(void*, const u8*, const u16*, u8** out) {
  if (out) *out = nullptr;
  return E_NOTIMPL_;
}
int TRT_GUEST_ABI di_EnumDevicesBySemantics(void*, const u16*, void*, void*,
                                            void*, u32) {
  return E_NOTIMPL_;
}
int TRT_GUEST_ABI di_ConfigureDevices(void*, void*, void*, u32, void*) {
  return E_NOTIMPL_;
}

// ---- IDirectInputDevice8 ----
int TRT_GUEST_ABI dd_GetCapabilities(void* self, void* caps) {
  dinput_device* d = (dinput_device*)self;
  if (!caps) return E_NOTIMPL_;
  memset(caps, 0, 44);
  u32* c = (u32*)caps;
  c[0] = 44u;  // dwSize = sizeof(DIDEVCAPS)
  c[1] = 0x00000001u;  // DIDC_ATTACHED
  if (d->kind == DEV_KEYBOARD) {
    c[2] = 0x00000413u;
    c[4] = 256u;  // dwButtons
  } else if (d->kind == DEV_MOUSE) {
    c[2] = 0x00000212u;
    c[3] = 3u;    // dwAxes
    c[4] = 5u;    // dwButtons
  } else {
    c[2] = 0x00010215u;  // DI8DEVTYPE_GAMEPAD
    c[3] = 6u;    // dwAxes
    c[4] = 14u;   // dwButtons
    c[5] = 1u;    // dwPOVs
  }
  return DI_OK_;
}
int TRT_GUEST_ABI dd_EnumObjects(void* self, void* cb, void* ctx, u32 flags) {
  (void)flags;
  if (!cb) return E_NOTIMPL_;
  dinput_device* d = (dinput_device*)self;
  if (d->kind != DEV_GAMEPAD) return DI_OK_;
  using enum_obj_cb = int (TRT_GUEST_ABI*)(const void*, void*);
  u8 doi[580];
  auto emit_obj = [&](u32 type_ofs, u32 flags_obj, const char* name) -> int {
    memset(doi, 0, sizeof(doi));
    u32 sz = 580u;  // sizeof(DIDEVICEOBJECTINSTANCEW)
    memcpy(doi + 0, &sz, 4);
    memcpy(doi + 20, &type_ofs, 4);  // dwOfs
    memcpy(doi + 24, &flags_obj, 4); // dwType
    u16* wn = (u16*)(doi + 32);
    for (int i = 0; name[i] && i < 259; ++i) wn[i] = (u16)(u8)name[i];
    return ((enum_obj_cb)cb)(doi, ctx);
  };
  emit_obj(0, 0x00000003u /*DIDFT_AXIS*/, "X Axis");
  emit_obj(4, 0x00000103u, "Y Axis");
  emit_obj(8, 0x00000203u, "Z Axis");
  emit_obj(12, 0x00000303u, "Rx Axis");
  emit_obj(16, 0x00000403u, "Ry Axis");
  emit_obj(20, 0x00000503u, "Rz Axis");
  emit_obj(32, 0x00000010u /*DIDFT_POV*/, "POV Hat");
  return DI_OK_;
}
int TRT_GUEST_ABI dd_GetProperty(void*, const u8*, void* prop) {
  if (prop) {
    // DIPROPRANGE: header(16) + lMin, lMax -> standard 0..65535
    u32 sz = *(u32*)prop;
    if (sz == 24u) {
      i32* r = (i32*)((u8*)prop + 16);
      r[0] = 0;
      r[1] = 65535;
    }
  }
  return DI_OK_;
}
int TRT_GUEST_ABI dd_SetProperty(void*, const u8*, const void*) {
  return DI_OK_;
}
int TRT_GUEST_ABI dd_Acquire(void* self) {
  ((dinput_device*)self)->acquired = 1;
  return DI_OK_;
}
int TRT_GUEST_ABI dd_Unacquire(void* self) {
  ((dinput_device*)self)->acquired = 0;
  return DI_OK_;
}

static i32 norm_di_axis(i16 v, bool invert) {
  i32 s = invert ? -(i32)v : (i32)v;
  i32 out = s + 32768;
  if (out < 0) out = 0;
  if (out > 65535) out = 65535;
  return out;
}

// GetDeviceState: the state a game actually polls
int TRT_GUEST_ABI dd_GetDeviceState(void* self, u32 size, void* buf) {
  dinput_device* d = (dinput_device*)self;
  if (!buf) return E_NOTIMPL_;
  if (d->kind == DEV_MOUSE) {
    if (size < sizeof(mouse_state)) return E_NOTIMPL_;
    mouse_state st;
    int dx = 0, dy = 0;
    trt_mouse_delta(&dx, &dy);           // consumes the accumulated motion
    st.lX = dx;
    st.lY = dy;
    st.lZ = trt_mouse_wheel();           // consumes
    memset(st.rgbButtons, 0, sizeof(st.rgbButtons));
    if (trt_mouse_button(0)) st.rgbButtons[0] = 0x80;
    if (trt_mouse_button(1)) st.rgbButtons[1] = 0x80;
    if (trt_mouse_button(2)) st.rgbButtons[2] = 0x80;
    if (trt_mouse_button(3)) st.rgbButtons[3] = 0x80;
    if (trt_mouse_button(4)) st.rgbButtons[4] = 0x80;
    memcpy(buf, &st, sizeof(st));
    return DI_OK_;
  }
  if (d->kind == DEV_GAMEPAD) {
    if (size < 80u) return E_NOTIMPL_;  // sizeof(DIJOYSTATE) == 80
    joy_state j{};
    j.lX = norm_di_axis(trt_pad_thumb_n(0, 0), false);
    j.lY = norm_di_axis(trt_pad_thumb_n(0, 1), true);
    j.lZ = 32768 + ((i32)trt_pad_trigger_n(0, 0) - (i32)trt_pad_trigger_n(0, 1)) * 128;
    if (j.lZ < 0) j.lZ = 0;
    if (j.lZ > 65535) j.lZ = 65535;
    j.lRx = norm_di_axis(trt_pad_thumb_n(0, 2), false);
    j.lRy = norm_di_axis(trt_pad_thumb_n(0, 3), true);
    j.lRz = (i32)trt_pad_trigger_n(0, 1) * 257;
    const bool du = trt_pad_down_n(0, 0x0001u);
    const bool dd = trt_pad_down_n(0, 0x0002u);
    const bool dl = trt_pad_down_n(0, 0x0004u);
    const bool dr = trt_pad_down_n(0, 0x0008u);
    u32 pov = 0xFFFFFFFFu;
    if (du && dr) pov = 4500u; else if (dr && dd) pov = 13500u;
    else if (dd && dl) pov = 22500u; else if (dl && du) pov = 31500u;
    else if (du) pov = 0u; else if (dr) pov = 9000u;
    else if (dd) pov = 18000u; else if (dl) pov = 27000u;
    j.rgdwPOV[0] = pov;
    j.rgdwPOV[1] = j.rgdwPOV[2] = j.rgdwPOV[3] = 0xFFFFFFFFu;
    static const u16 k_xi_btns[10] = {
        0x1000u, 0x2000u, 0x4000u, 0x8000u, 0x0100u,
        0x0200u, 0x0020u, 0x0010u, 0x0040u, 0x0080u};
    for (int i = 0; i < 10; ++i) {
      if (trt_pad_down_n(0, k_xi_btns[i])) j.rgbButtons[i] = 0x80u;
    }
    if (trt_pad_trigger_n(0, 0) > 64) j.rgbButtons[10] = 0x80u;
    if (trt_pad_trigger_n(0, 1) > 64) j.rgbButtons[11] = 0x80u;
    const size_t copy_sz = size < sizeof(j) ? (size_t)size : sizeof(j);
    memcpy(buf, &j, copy_sz);
    return DI_OK_;
  }
  // Keyboard state is indexed by DirectInput scan code. Keep a small,
  // rate-limited trace so remote keyboard failures can be distinguished from
  // Unity/game-state stalls without flooding the console log.
  if (size < 256) return E_NOTIMPL_;
  u8 state[256]{};
  trt_input_copy_dik_state(state);
  static std::atomic<unsigned> keyboard_state_reads{0};
  const unsigned read_no = keyboard_state_reads.fetch_add(1, std::memory_order_relaxed);
  if (read_no < 4 || state[0x1c] || state[0x39] || state[0x2c]) {
    char line[160];
    snprintf(line, sizeof(line),
                  "dinput: keyboard GetDeviceState #%u return=%02x space=%02x z=%02x",
                  read_no + 1, state[0x1c], state[0x39], state[0x2c]);
    log_line(line);
  }
  memcpy(buf, state, sizeof(state));
  return DI_OK_;
}

int TRT_GUEST_ABI dd_GetDeviceData(void*, u32, void*, u32*, u32) {
  return E_NOTIMPL_;
}
int TRT_GUEST_ABI dd_SetDataFormat(void*, const void*) { return DI_OK_; }
int TRT_GUEST_ABI dd_SetEventNotification(void*, void*) { return DI_OK_; }
int TRT_GUEST_ABI dd_SetCooperativeLevel(void* self, void* hwnd, u32 level) {
  ((dinput_device*)self)->coop_level = level;
  return DI_OK_;
}
int TRT_GUEST_ABI dd_GetObjectInfo(void*, void*, u32, u32) { return E_NOTIMPL_; }
int TRT_GUEST_ABI dd_GetDeviceInfo(void* self, void* info) {
  auto* d = (dinput_device*)self;
  if (!d || !info) return E_NOTIMPL_;
  const u8* guid = k_guid_mouse;
  u32 dev_type = 0x00000212u;
  const char* name = "Tiramisu Mouse";
  if (d->kind == DEV_KEYBOARD) {
    guid = k_guid_keyboard;
    dev_type = 0x00000413u;
    name = "Tiramisu Keyboard";
  } else if (d->kind == DEV_GAMEPAD) {
    guid = k_guid_gamepad;
    dev_type = 0x00010215u;
    name = "Tiramisu Xbox Gamepad";
  }
  fill_di_dev_inst(info, guid, dev_type, name);
  return DI_OK_;
}
int TRT_GUEST_ABI dd_RunControlPanel(void*, void*, u32) { return E_NOTIMPL_; }
int TRT_GUEST_ABI dd_Initialize(void*, void*, u32, const u8*) { return DI_OK_; }
int TRT_GUEST_ABI dd_CreateEffect(void*, const u8*, const void*, void** out,
                                  void*) {
  if (out) *out = nullptr;
  return E_NOTIMPL_;
}
int TRT_GUEST_ABI dd_EnumEffects(void*, void*, void*, u32) { return E_NOTIMPL_; }
int TRT_GUEST_ABI dd_GetEffectInfo(void*, void*, const u8*) { return E_NOTIMPL_; }
int TRT_GUEST_ABI dd_GetForceFeedbackState(void*, u32* state) {
  if (state) *state = 0;  // no force feedback on the console
  return DI_OK_;
}
int TRT_GUEST_ABI dd_SendForceFeedbackCommand(void* self, u32 cmd) {
  auto* d = (dinput_device*)self;
  // DISFFC_RESET (1) or DISFFC_STOPALL (2)
  if (d && d->kind == DEV_GAMEPAD && (cmd == 1u || cmd == 2u)) {
    trt_pad_set_vibration(0, 0, 0);
  }
  return DI_OK_;
}
int TRT_GUEST_ABI dd_EnumCreatedEffectObjects(void*, void*, void*, u32) {
  return E_NOTIMPL_;
}
int TRT_GUEST_ABI dd_Escape(void*, void*) { return E_NOTIMPL_; }
int TRT_GUEST_ABI dd_Poll(void*) { return DI_OK_; }
int TRT_GUEST_ABI dd_SendDeviceData(void*, u32, const void*, u32*, u32) {
  return E_NOTIMPL_;
}
int TRT_GUEST_ABI dd_EnumEffectsInFile(void*, const u16*, void*, void*, u32) {
  return E_NOTIMPL_;
}
int TRT_GUEST_ABI dd_WriteEffectToFile(void*, const u16*, u32, void*, u32) {
  return E_NOTIMPL_;
}
int TRT_GUEST_ABI dd_BuildActionMap(void*, void*, const u16*, u32) {
  return E_NOTIMPL_;
}
int TRT_GUEST_ABI dd_SetActionMap(void*, void*, const u16*, u32) {
  return E_NOTIMPL_;
}
int TRT_GUEST_ABI dd_GetImageInfo(void*, void*) { return E_NOTIMPL_; }


dinput_object g_di8_obj = {g_di8_vtbl, 1};
dinput_device g_dev_keyboard = {g_didev_vtbl, 1, DEV_KEYBOARD, 0, 0};
dinput_device g_dev_mouse = {g_didev_vtbl, 1, DEV_MOUSE, 0, 0};
dinput_device g_dev_gamepad = {g_didev_vtbl, 1, DEV_GAMEPAD, 0, 0};

// the objects and the vtables are defined below
extern const void* g_di8_vtbl[11];
extern const void* g_didev_vtbl[34];

// IUnknown (defined below, shared by both objects)
int di_QueryInterface(void* self, const u8* riid, void** out);
u32 di_AddRef(void* self);
u32 di_Release(void* self);

// ---- IDirectInput8W (slots 3..10) ----
int di_CreateDevice(void* self, const u8* guid, void** out_device,
                    void* unk);
int di_EnumDevices(void* self, u32 type, void* callback, void* context,
                   u32 flags);
int di_GetDeviceStatus(void* self, const u8* guid);
int di_RunControlPanel(void* self, void* hwnd, u32 flags);
int di_Initialize(void* self, void* hinst, u32 version);
int di_FindDevice(void* self, const u8* guid, const u16* name, u8** out);
int di_EnumDevicesBySemantics(void* self, const u16* semantics, void* fmt,
                              void* callback, void* context, u32 flags);
int di_ConfigureDevices(void* self, void* callback, void* params, u32 flags,
                        void* context);

// IUnknown + the eight IDirectInput8 methods
const void* g_di8_vtbl[11] = {
    (const void*)&di_QueryInterface,
    (const void*)&di_AddRef,
    (const void*)&di_Release,
    (const void*)&di_CreateDevice,
    (const void*)&di_EnumDevices,
    (const void*)&di_GetDeviceStatus,
    (const void*)&di_RunControlPanel,
    (const void*)&di_Initialize,
    (const void*)&di_FindDevice,
    (const void*)&di_EnumDevicesBySemantics,
    (const void*)&di_ConfigureDevices,
};

// ---- IDirectInputDevice8W (slots 3..33) ----
int dd_GetCapabilities(void* self, void* caps);
int dd_EnumObjects(void* self, void* callback, void* context, u32 flags);
int dd_GetProperty(void* self, const u8* guid, void* header);
int dd_SetProperty(void* self, const u8* guid, const void* header);
int dd_Acquire(void* self);
int dd_Unacquire(void* self);
int dd_GetDeviceState(void* self, u32 size, void* buf);
int dd_GetDeviceData(void* self, u32 size, void* data, u32* count, u32 flags);
int dd_SetDataFormat(void* self, const void* format);
int dd_SetEventNotification(void* self, void* handle);
int dd_SetCooperativeLevel(void* self, void* hwnd, u32 level);
int dd_GetObjectInfo(void* self, void* info, u32 size, u32 flags);
int dd_GetDeviceInfo(void* self, void* info);
int dd_RunControlPanel(void* self, void* hwnd, u32 flags);
int dd_Initialize(void* self, void* hinst, u32 version, const u8* guid);
int dd_CreateEffect(void* self, const u8* guid, const void* desc,
                    void** out, void* unk);
int dd_EnumEffects(void* self, void* callback, void* context, u32 flags);
int dd_GetEffectInfo(void* self, void* info, const u8* guid);
int dd_GetForceFeedbackState(void* self, u32* state);
int dd_SendForceFeedbackCommand(void* self, u32 cmd);
int dd_EnumCreatedEffectObjects(void* self, void* callback, void* context,
                                u32 flags);
int dd_Escape(void* self, void* escape);
int dd_Poll(void* self);
int dd_SendDeviceData(void* self, u32 size, const void* data, u32* count,
                      u32 flags);
int dd_EnumEffectsInFile(void* self, const u16* file, void* callback,
                         void* context, u32 flags);
int dd_WriteEffectToFile(void* self, const u16* file, u32 size, void* data,
                         u32 flags);
int dd_BuildActionMap(void* self, void* format, const u16* path, u32 flags);
int dd_SetActionMap(void* self, void* format, const u16* path, u32 flags);
int dd_GetImageInfo(void* self, void* info);

const void* g_didev_vtbl[34] = {
    (const void*)&di_QueryInterface,
    (const void*)&di_AddRef,
    (const void*)&di_Release,
    (const void*)&dd_GetCapabilities,
    (const void*)&dd_EnumObjects,
    (const void*)&dd_GetProperty,
    (const void*)&dd_SetProperty,
    (const void*)&dd_Acquire,
    (const void*)&dd_Unacquire,
    (const void*)&dd_GetDeviceState,
    (const void*)&dd_GetDeviceData,
    (const void*)&dd_SetDataFormat,
    (const void*)&dd_SetEventNotification,
    (const void*)&dd_SetCooperativeLevel,
    (const void*)&dd_GetObjectInfo,
    (const void*)&dd_GetDeviceInfo,
    (const void*)&dd_RunControlPanel,
    (const void*)&dd_Initialize,
    (const void*)&dd_CreateEffect,
    (const void*)&dd_EnumEffects,
    (const void*)&dd_GetEffectInfo,
    (const void*)&dd_GetForceFeedbackState,
    (const void*)&dd_SendForceFeedbackCommand,
    (const void*)&dd_EnumCreatedEffectObjects,
    (const void*)&dd_Escape,
    (const void*)&dd_Poll,
    (const void*)&dd_SendDeviceData,
    (const void*)&dd_EnumEffectsInFile,
    (const void*)&dd_WriteEffectToFile,
    (const void*)&dd_BuildActionMap,
    (const void*)&dd_SetActionMap,
    (const void*)&dd_GetImageInfo,
};

} // namespace

extern "C" {

// ---- guest-side input feed (guest ABI: the guest calls these directly) ----
void TRT_GUEST_ABI trt_di_feed_move(i32 dx, i32 dy) {
  trt_input_mouse_delta_raw(dx, dy);
}
void TRT_GUEST_ABI trt_di_feed_button(i32 btn, u32 down) {
  trt_input_mouse_button((int)btn, down != 0);
}
// The argument is a DirectInput scan code (DIK_*), matching the layout of a
// real IDirectInputDevice8 keyboard state (DIK_A = 0x1E, DIK_Z = 0x2C;
// bit 7 marks E0-extended keys). The state this bridge hands out is indexed
// by scan code, so the feed must be translated to (vk, scan) or a fed key
// would never appear in GetDeviceState.
void TRT_GUEST_ABI trt_di_feed_key(u16 dik, u32 down) {
  const u16 scan = (u16)(dik & 0x7FU);
  const u8 extended = (dik & 0x80U) ? 1 : 0;
  const u32 vsc = (u32)scan | (extended ? 0xE000U : 0U);
  u16 vk = (u16)(trt_MapVirtualKeyW(vsc, 3 /*MAPVK_VSC_TO_VK_EX*/) & 0xFFU);
  if (!vk) vk = dik ? dik : scan;  // unknown scan: keep the key visible
  trt_input_key_event2(vk, down != 0, scan, extended, 0);
}

// DirectInput8Create(hinst, version, riid, ppv, unk)
int TRT_GUEST_ABI trt_DirectInput8Create(void* hinst, u32 version,
                                         const u8* riid, void** out,
                                         void* unk) {
  gap_trace_record("DirectInput8Create");
  (void)hinst; (void)version; (void)riid; (void)unk;
  if (!out) return E_NOTIMPL_;
  *out = &g_di8_obj;
  return DI_OK_;
}

} // extern "C"

// CLSID_DirectInput8 {25E609E4-B259-11CF-BFC7-444553540000} — the COM path
// to the same object DirectInput8Create hands out. Games created through
// CoCreateInstance (Unity input plugins, SDL windows) were refused with
// REGDB_E_CLASSNOTREG even though the dll surface existed (measured: the
// corpus gap list named this CLSID).
// NOTE: factories are invoked by the HOST ole bridge (plain SysV); only the
// vtbl methods of the returned object are TRT_GUEST_ABI.
int dinput8_com_factory(const void* iid, void** out, void* user) {
  (void)user;
  if (!iid || !out) return (int)0x80070057;  // E_INVALIDARG
  *out = &g_di8_obj;
  char line[128];
  // IID_IDirectInput8W  {1237BF00-12BE-101C-9B1D-08002BB30F25}
  // IID_IDirectInput8A  {1237BF01-12BE-101C-9B1D-08002BB30F25}
  static const u8 k_iid_di8w[16] = {0x00, 0xBF, 0x37, 0x12, 0xBE, 0x12, 0x1C,
                                    0x10, 0x9B, 0x1D, 0x08, 0x00, 0x2B, 0xB3,
                                    0x0F, 0x25};
  static const u8 k_iid_di8a[16] = {0x01, 0xBF, 0x37, 0x12, 0xBE, 0x12, 0x1C,
                                    0x10, 0x9B, 0x1D, 0x08, 0x00, 0x2B, 0xB3,
                                    0x0F, 0x25};
  const bool w = memcmp(iid, k_iid_di8w, 16) == 0;
  const bool a = memcmp(iid, k_iid_di8a, 16) == 0;
  if (w || a) {
    snprintf(line, sizeof(line),
             "dinput8: CoCreateInstance(CLSID_DirectInput8, IID_IDirectInput8%c)"
             " -> IDirectInput8W object (A-twin served by the W vtable)",
             a ? 'A' : 'W');
    log_line(line);
    return DI_OK_;
  }
  snprintf(line, sizeof(line),
           "dinput8: CoCreateInstance(CLSID_DirectInput8) -> E_NOINTERFACE "
           "(IID not IDirectInput8W/A)");
  log_line(line);
  return (int)0x80004002;  // E_NOINTERFACE
}

extern "C" int trt_bridge_register_dinput(void) {
  // CLSID_DirectInput8 {25E609E4-B259-11CF-BFC7-444553540000}
  static const u8 k_clsid_dinput8[16] = {0xE4, 0x09, 0xE6, 0x25, 0x59, 0xB2,
                                         0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45,
                                         0x53, 0x54, 0x00, 0x00};
  trt_ole_register_class(k_clsid_dinput8, &dinput8_com_factory, nullptr);

  static const trt_named_fn k_fns[] = {
      {"DirectInput8Create", (void*)&trt_DirectInput8Create},
      {"trt_di_feed_move", (void*)&trt_di_feed_move},
      {"trt_di_feed_button", (void*)&trt_di_feed_button},
      {"trt_di_feed_key", (void*)&trt_di_feed_key},
  };
  trt_module_register("dinput8.dll", TRT_ROUTE_BUILTIN, k_fns,
                       (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  trt_module_register("dinput.dll", TRT_ROUTE_BUILTIN, k_fns,
                       (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  return 0;
}

} // extern "C"
