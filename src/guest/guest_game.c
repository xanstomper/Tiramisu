// tiramisu/src/guest/guest_game.c
// Game-shaped boot guest: everything a real game does at startup, importing
// ONLY from the builtin bridges (kernel32 + user32) so the identical binary
// runs through the full loader on the Linux host AND on console.
//   RegisterClassExW -> CreateWindowExW -> PeekMessage pump -> WndProc called
//   GetAsyncKeyState + GetCursorPos + HeapAlloc + QPC + env strings + module
//   path + CreateFileW(C:\results.txt) -> WriteFile -> CloseHandle
// Returns a checksum; the host test independently computes the expectation.
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned long long u64;
typedef long long i64;
typedef int i32;
typedef short i16;
typedef unsigned char u8;

#ifdef _WIN32
#define GSTD __stdcall
#else
#define GSTD
#endif

// kernel32 imports
__attribute__((dllimport)) void* GSTD GetProcessHeap(void);
__attribute__((dllimport)) void* GSTD HeapAlloc(void* heap, u32 flags, u64 bytes);
__attribute__((dllimport)) int GSTD QueryPerformanceCounter(i64* out);
__attribute__((dllimport)) void* GSTD CreateFileW(const u16* name, u32 access,
                                                  u32 share, void* sa,
                                                  u32 disp, u32 flags,
                                                  void* tmpl);
__attribute__((dllimport)) int GSTD WriteFile(void* h, const void* buf,
                                              u32 bytes, u32* written,
                                              void* ov);
__attribute__((dllimport)) int GSTD CloseHandle(void* h);
__attribute__((dllimport)) const u16* GSTD GetEnvironmentStringsW(void);
__attribute__((dllimport)) u32 GSTD GetModuleFileNameW(void* mod, u16* buf,
                                                       u32 len);
// user32 imports
__attribute__((dllimport)) u16 GSTD RegisterClassExW(const void* wcx);
__attribute__((dllimport)) void* GSTD CreateWindowExW(u32 ex, const u16* cls,
                                                      const u16* title,
                                                      u32 style, int x, int y,
                                                      int w, int h, void* p,
                                                      void* menu, void* inst,
                                                      void* param);
__attribute__((dllimport)) int GSTD PeekMessageW(void* msg, void* hwnd,
                                                 u32 min, u32 max, u32 remove);
__attribute__((dllimport)) int GSTD TranslateMessage(const void* msg);
__attribute__((dllimport)) i64 GSTD DispatchMessageW(const void* msg);
__attribute__((dllimport)) i64 GSTD SendMessageW(void* hwnd, u32 msg, u64 wp,
                                                 i64 lp);
__attribute__((dllimport)) i16 GSTD GetAsyncKeyState(int vk);
__attribute__((dllimport)) int GSTD GetCursorPos(void* pt);

static volatile u32 g_wndproc_calls = 0;
static u32 g_last_msg = 0;
static void* g_expected_hwnd = (void*)0;
static u32 g_abi_ok = 0;

static i64 GSTD wndproc(void* hwnd, u32 msg, u64 wp, i64 lp) {
  if (msg == 0x8007u && hwnd == g_expected_hwnd &&
      wp == 0x1122334455667788ULL &&
      (u64)lp == 0x99AABBCCDDEEFF01ULL) {
    g_abi_ok = 1;
    return (i64)0x0BADF00DDEADBEEFULL;
  }
  g_wndproc_calls++;
  g_last_msg = msg;
  return msg == 0x0081u /*WM_NCCREATE*/ ? 1 : 0;
}

// winuser MSG (x64): hwnd, message, pad, wParam, lParam, time, pt = 48
typedef struct { u64 hwnd; u32 message; u32 pad; u64 wp; i64 lp;
                 u32 time; i32 pt[2]; } MSG64;
// WNDCLASSEXW head we use: cbSize, style, proc, extras..., name
typedef struct { u32 cbSize; u32 style; void* proc; i32 cx; i32 wx;
                 void* inst; void* icon; void* cursor; void* brush;
                 const u16* menu; const u16* name; } WNDCLASSEX64;

u32 GSTD game_boot(u64 a, u64 b, u64 c) {
  (void)a; (void)b; (void)c;

  // 1. class + window
  static const u16 cls_name[] = {'G','a','m','e','B','o','o','t',0};
  static const u16 win_title[] = {'T','i','r','a','m','i','s','u',' ','b','o','o','t',0};
  WNDCLASSEX64 wc;
  __builtin_memset(&wc, 0, sizeof(wc));
  wc.cbSize = (u32)sizeof(WNDCLASSEX64);
  wc.proc = (void*)&wndproc;
  wc.name = cls_name;
  const u16 atom = RegisterClassExW(&wc);
  void* hwnd = CreateWindowExW(0, cls_name, win_title, 0, 0, 0, 320, 240,
                               (void*)0, (void*)0, (void*)0, (void*)0);
  g_expected_hwnd = hwnd;
  const i64 sret = hwnd
      ? SendMessageW(hwnd, 0x8007u, 0x1122334455667788ULL,
                     (i64)0x99AABBCCDDEEFF01ULL)
      : 0;

  // 2. the standard game pump (PeekMessage + Translate + Dispatch)
  MSG64 m;
  u32 pumped = 0;
  for (int frame = 0; frame < 8; ++frame) {
    while (PeekMessageW(&m, (void*)0, 0, 0, 1)) {
      TranslateMessage(&m);
      DispatchMessageW(&m);
      ++pumped;
      if (pumped > 16) break;
    }
  }

  // 3. input snapshot (no events fed: deterministic)
  const i16 key = GetAsyncKeyState('W');
  struct { i32 x, y; } pt = {0, 0};
  GetCursorPos(&pt);
  const u32 cursor_ran = pt.x >= 0 && pt.y >= 0;

  // 4. heap + timer
  void* mem = HeapAlloc(GetProcessHeap(), 0x8 /*HEAP_ZERO_MEMORY*/, 256);
  i64 qpc = 0;
  QueryPerformanceCounter(&qpc);

  // 5. environment: COMPUTERNAME=TIRAMISU (block = strings separated by NUL,
  // terminated by an empty string)
  const u16* env = GetEnvironmentStringsW();
  u32 env_ok = 0;
  for (const u16* p = env; ; ++p) {
    if (p[0]=='C' && p[1]=='O' && p[2]=='M' && p[3]=='P' && p[4]=='U') {
      const u16* v = p;
      while (*v && *v != '=') ++v;
      if (*v == '=' && v[1]=='T' && v[2]=='I' && v[3]=='R') { env_ok = 1; break; }
    }
    if (p[0] == 0 && p[1] == 0) break;  // end of the block
  }

  // 6. module path must be the Z:\ virtual drive
  u16 modpath[260];
  const u32 modlen = GetModuleFileNameW((void*)0, modpath, 260);
  const u32 mod_ok = modlen > 2 && modpath[0] == 'Z' && modpath[1] == ':';

  // 7. write results through the VFS: C:\results.txt
  // every term is a distinct BIT OR-ed in (no carries: host asserts exactly)
  u32 checksum = 0x40000000u;
  checksum |= (atom != 0) ? 0x8000u : 0;    // class registered
  checksum |= (hwnd != (void*)0) ? 0x100 : 0;
  checksum |= pumped * 0x10;                // pump ran, messages dispatched
  checksum |= (g_wndproc_calls > 0) ? 0x1000 : 0;
  checksum |= (key == 0) ? 0x10000 : 0;     // no key down (deterministic)
  checksum |= (mem != (void*)0) ? 0x100000 : 0;
  checksum |= (qpc != 0) ? 0x200000 : 0;
  checksum |= env_ok ? 0x400000 : 0;
  checksum |= mod_ok ? 0x800000 : 0;
  checksum |= cursor_ran ? 0x1000000 : 0;
  checksum |= (g_abi_ok && (u64)sret == 0x0BADF00DDEADBEEFULL) ? 0x2000000 : 0;

  void* f = CreateFileW(
      (const u16[]){'C',':','\\','r','e','s','u','l','t','s','.','t','x','t',0},
      0x40000000u /*GENERIC_WRITE*/, 0, (void*)0, 2 /*CREATE_ALWAYS*/, 0,
      (void*)0);
  if (f != (void*)-1 && f) {
    u32 written = 0;
    const u32 sum = checksum;               // (drop volatile)
    WriteFile(f, &sum, 4, &written, (void*)0);
    const u32 calls = g_wndproc_calls;
    WriteFile(f, &calls, 4, &written, (void*)0);
    CloseHandle(f);
  }
  return checksum;
}
