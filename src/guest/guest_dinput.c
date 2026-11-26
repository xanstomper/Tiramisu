// tiramisu/src/guest/guest_dinput.c
// The DirectInput path a shipping game uses:
//   DirectInput8Create -> CreateDevice(mouse) -> SetDataFormat ->
//   SetCooperativeLevel -> Acquire -> Poll -> GetDeviceState (DIMOUSESTATE)
//   and the 256-byte keyboard state.
// Returns a bitmask of what worked.
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
typedef long long i64;
typedef unsigned long long u64;
typedef int i32;

#ifdef _WIN32
#define GSTD __stdcall
#else
#define GSTD
#endif

__attribute__((dllimport)) i32 GSTD DirectInput8Create(void* hinst, u32 ver,
                                                       const u8* riid,
                                                       void** out, void* unk);
// IDirectInput8
__attribute__((dllimport)) i32 GSTD di_CreateDevice(const u8* guid, void** out,
                                                    void* unk);
__attribute__((dllimport)) i32 GSTD di_Initialize(void* hinst, u32 ver);
// IDirectInputDevice8
__attribute__((dllimport)) i32 GSTD dd_SetDataFormat(void* self,
                                                     const void* fmt);
__attribute__((dllimport)) i32 GSTD dd_SetCooperativeLevel(void* self,
                                                           void* hwnd,
                                                           u32 level);
__attribute__((dllimport)) i32 GSTD dd_Acquire(void* self);
__attribute__((dllimport)) i32 GSTD dd_Poll(void* self);
__attribute__((dllimport)) i32 GSTD dd_GetDeviceState(void* self, u32 size,
                                                      void* buf);
__attribute__((dllimport)) u32 GSTD dd_Release(void* self);
// guest-ABI feed wrappers (the dinput8 bridge exports them)
__attribute__((dllimport)) void GSTD trt_di_feed_move(i32 dx, i32 dy);
__attribute__((dllimport)) void GSTD trt_di_feed_button(int btn, u32 down);
__attribute__((dllimport)) void GSTD trt_di_feed_key(u16 dik, u32 down);

// the guest's COM calls go through the object pointers DirectInput8Create
// returned: the vtable is read from memory, like real COM.
static void* g_di8 = 0;
static void* g_mouse = 0;
static void* g_keyboard = 0;

// read slot N of a COM object's vtable (after the 3 IUnknown slots)
static void* vmethod(void* obj, u32 slot) {
  void** vtbl = *(void***)obj;
  return vtbl[slot];
}

u32 GSTD dinput_boot(u64 a, u64 b, u64 c) {
  (void)a; (void)b; (void)c;
  u32 bits = 0;

  // 1. create the factory
  const u8 iid[16] = {0x31, 0x80, 0x79, 0xBF, 0x3A, 0x48, 0xA2, 0x4D,
                      0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00};
  if (DirectInput8Create((void*)0x400000, 0x800, iid, &g_di8, 0) == 0 && g_di8)
    bits |= 0x1u;

  // 2. the mouse device (GUID_SysMouse)
  const u8 guid_mouse[16] = {0x60, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11,
                             0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00};
  if (g_di8 && vmethod(g_di8, 3) &&
      ((i32 GSTD (*)(void*, const u8*, void**, void*))vmethod(g_di8, 3))(
          g_di8, guid_mouse, &g_mouse, 0) == 0 &&
      g_mouse)
    bits |= 0x2u;

  // 3. data format + cooperative level + acquire
  if (g_mouse &&
      ((i32 GSTD (*)(void*, const void*))vmethod(g_mouse, 11))(g_mouse, 0) == 0)
    bits |= 0x4u;
  if (g_mouse &&
      ((i32 GSTD (*)(void*, void*, u32))vmethod(g_mouse, 13))(g_mouse,
                                                              (void*)0x1234,
                                                              6) == 0)
    bits |= 0x8u;
  if (g_mouse && ((i32 GSTD (*)(void*))vmethod(g_mouse, 7))(g_mouse) == 0)
    bits |= 0x10u;

  // 4. a polled mouse state carries the deltas the host fed
  // DIMOUSESTATE: dx, dy, dz (i32), rgbButtons[8]
  struct { i32 dx, dy, dz; u8 buttons[8]; } ms;
  if (g_mouse) {
    trt_di_feed_move(13, -9);
    trt_di_feed_button(0, 1);
    ((i32 GSTD (*)(void*))vmethod(g_mouse, 25))(g_mouse);  // Poll
    if (((i32 GSTD (*)(void*, u32, void*))vmethod(g_mouse, 9))(
            g_mouse, sizeof(ms), &ms) == 0) {
      const int ok = ms.dx == 13 && ms.dy == -9 && ms.buttons[0] == 0x80;
      bits |= ok ? 0x20u : 0u;
    }
    trt_di_feed_button(0, 0);
  }

  // 5. the keyboard state (256 bytes, one per virtual key)
  const u8 guid_kbd[16] = {0x61, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11,
                           0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00};
  if (g_di8 &&
      ((i32 GSTD (*)(void*, const u8*, void**, void*))vmethod(g_di8, 3))(
          g_di8, guid_kbd, &g_keyboard, 0) == 0 &&
      g_keyboard) {
    u8 keys[256];
    trt_di_feed_key(0x1E, 1);  // A down
    if (((i32 GSTD (*)(void*, u32, void*))vmethod(g_keyboard, 9))(
            g_keyboard, 256, keys) == 0) {
      bits |= (keys[0x1E] == 0x80 && keys[0x2C] == 0) ? 0x40u : 0u;
    }
    trt_di_feed_key(0x1E, 0);
    if (((i32 GSTD (*)(void*, u32, void*))vmethod(g_keyboard, 9))(
            g_keyboard, 256, keys) == 0) {
      bits |= (keys[0x1E] == 0) ? 0x80u : 0u;
    }
  }

  // 6. release counts down to zero (Windows returns the remaining count)
  if (g_mouse && ((u32 GSTD (*)(void*))vmethod(g_mouse, 2))(g_mouse) == 0)
    bits |= 0x100u;

  return bits;
}
