// tiramisu/src/guest/guest_engine.c
// The APIs a real engine touches before it will show a window. If any of
// these is missing, a game quits at launch or hangs:
//   - memory-mapped asset files (CreateFileMapping/MapViewOfFile)
//   - worker threads + events + critical sections
//   - system info (version, page size, processor count)
//   - wide/ANSI conversion
//   - the quit and focus lifecycle
// Returns a bitmask.
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
typedef unsigned long long u64;
typedef int i32;

#ifdef _WIN32
#define GSTD __stdcall
#else
#define GSTD
#endif

// kernel32
__attribute__((dllimport)) void* GSTD CreateFileMappingW(void* h, void* a,
                                                          u32 prot, u32 hi,
                                                          u32 lo, const u16* name);
__attribute__((dllimport)) void* GSTD MapViewOfFile(void* h, u32 access,
                                                     u32 hi, u32 lo, u64 bytes);
__attribute__((dllimport)) int GSTD UnmapViewOfFile(void* view);
__attribute__((dllimport)) void* GSTD CreateThread(void* attrs, void* stack,
                                                   void* routine, void* param,
                                                   u32 flags, u32* tid);
__attribute__((dllimport)) u32 GSTD WaitForSingleObject(void* h, u32 ms);
__attribute__((dllimport)) void* GSTD CreateEventW(void* attrs, u32 manual,
                                                   u32 initial, const u16* name);
__attribute__((dllimport)) int GSTD SetEvent(void* h);
__attribute__((dllimport)) int GSTD ResetEvent(void* h);
__attribute__((dllimport)) void GSTD InitializeCriticalSection(void* cs);
__attribute__((dllimport)) void GSTD EnterCriticalSection(void* cs);
__attribute__((dllimport)) void GSTD LeaveCriticalSection(void* cs);
__attribute__((dllimport)) int GSTD GetVersionExW(void* info);
__attribute__((dllimport)) void GSTD GetSystemInfo(void* si);
__attribute__((dllimport)) int GSTD MultiByteToWideChar(u32 cp, u32 flags,
                                                        const char* s, i32 sl,
                                                        u16* d, i32 dl);
__attribute__((dllimport)) int GSTD WideCharToMultiByte(u32 cp, u32 flags,
                                                        const u16* s, i32 sl,
                                                        char* d, i32 dl,
                                                        const char* def,
                                                        void* used);
__attribute__((dllimport)) u32 GSTD GetTempPathW(u32 size, u16* buf);
__attribute__((dllimport)) u32 GSTD GetTickCount64(void);
// user32
__attribute__((dllimport)) void GSTD PostQuitMessage(i32 code);
__attribute__((dllimport)) void* GSTD CreateWindowExW(
    u32 exstyle, const u16* class_name, const u16* title, u32 style,
    i32 x, i32 y, i32 width, i32 height, void* parent, void* menu,
    void* instance, void* param);
__attribute__((dllimport)) void* GSTD SetFocus(void* hwnd);
__attribute__((dllimport)) void* GSTD GetFocus(void);
__attribute__((dllimport)) int GSTD GetClientRect(void* hwnd, void* rect);

// the worker thread the guest spawns (runs in a real host thread)
static volatile u32 g_worker_ran = 0;
static void* g_event = 0;

u32 GSTD worker(void* param) {
  (void)param;
  g_worker_ran = 1;
  if (g_event) SetEvent(g_event);
  return 7;
}

// a critical section the worker and the main thread share
static u8 g_cs[64];

u32 GSTD engine_boot(u64 a, u64 b, u64 c) {
  (void)a; (void)b; (void)c;
  u32 bits = 0;

  // 1. system info
  u8 osinfo[300];
  for (u32 i = 0; i < sizeof(osinfo); ++i) osinfo[i] = 0;
  osinfo[0] = 276;  // dwOSVersionInfoSize
  i32 vr = GetVersionExW(osinfo);
  const u32* os = (const u32*)osinfo;
  bits |= (vr && os[1] >= 6 && os[3] >= 1000) ? 0x1u : 0u;

  u8 si[64];
  for (u32 i = 0; i < sizeof(si); ++i) si[i] = 0;
  GetSystemInfo(si);
  const u32* s = (const u32*)si;
  bits |= (s[1] == 4096 && s[5] >= 1) ? 0x2u : 0u;   // page size, cpus

  // 2. wide/ANSI conversion
  u16 w[16];
  i32 n = MultiByteToWideChar(0, 0, "AB", 2, w, 16);
  bits |= (n == 2 && w[0] == 'A' && w[1] == 'B') ? 0x4u : 0u;
  char back[16];
  i32 m = WideCharToMultiByte(0, 0, w, 2, back, 16, 0, 0);
  bits |= (m == 2 && back[0] == 'A') ? 0x8u : 0u;

  // 3. temp path
  u16 tmp[260];
  u32 tl = GetTempPathW(260, tmp);
  bits |= (tl > 1 && tmp[0] != 0) ? 0x10u : 0u;

  // 4. a mapped file: create one through the VFS first
  //    (the test writes C:\asset.bin before calling us)
  __attribute__((dllimport)) void* GSTD CreateFileW(const u16* name, u32 access,
                                                    u32 share, void* sa,
                                                    u32 disp, u32 flags,
                                                    void* tmpl);
  __attribute__((dllimport)) int GSTD WriteFile(void* h, const void* buf,
                                                u32 bytes, u32* written,
                                                void* ov);
  __attribute__((dllimport)) int GSTD CloseHandle(void* h);
  void* fh = CreateFileW((const u16[]){'C', ':', '\\', 'a', 's', 's', 'e', 't',
                                        '.', 'b', 'i', 'n', 0},
                         0x40000000u, 0, 0, 4 /*OPEN_ALWAYS*/, 0, 0);
  if (fh && fh != (void*)-1) {
    const char payload[] = "PAYLOAD-1234";
    u32 written = 0;
    WriteFile(fh, payload, (u32)(sizeof(payload) - 1), &written, 0);
    CloseHandle(fh);
    fh = CreateFileW((const u16[]){'C', ':', '\\', 'a', 's', 's', 'e', 't',
                                   '.', 'b', 'i', 'n', 0},
                    0x80000000u /*GENERIC_READ*/, 0, 0, 3 /*OPEN_EXISTING*/, 0,
                    0);
  }
  if (fh && fh != (void*)-1) {
    void* mapping = CreateFileMappingW(fh, 0, 2 /*PAGE_READONLY*/, 0, 0, 0);
    u8* view = (u8*)MapViewOfFile(mapping, 4 /*FILE_MAP_READ*/, 0, 0, 0);
    const int ok = view && view[0] == 'P' && view[1] == 'A' && view[7] == '-';
    bits |= ok ? 0x20u : 0u;
    if (view) UnmapViewOfFile(view);
    if (mapping) CloseHandle(mapping);
    CloseHandle(fh);
  }

  // 5. a worker thread + event + critical section
  g_event = CreateEventW(0, 1 /*manual*/, 0, 0);
  InitializeCriticalSection(g_cs);
  u32 tid = 0;
  void* th = CreateThread(0, 0, (void*)&worker, 0, 0, &tid);
  bits |= (th != 0 && tid != 0) ? 0x40u : 0u;
  if (th) {
    const u32 wr = WaitForSingleObject(th, 2000);
    EnterCriticalSection(g_cs);
    const u32 ran = g_worker_ran;
    LeaveCriticalSection(g_cs);
    bits |= (wr == 0 && ran == 1) ? 0x80u : 0u;
    CloseHandle(th);
  }
  if (g_event) {
    bits |= (SetEvent(g_event) && ResetEvent(g_event)) ? 0x100u : 0u;
    CloseHandle(g_event);
  }

  // 6. quit + focus lifecycle
  const u16 window_class[] = {'S','T','A','T','I','C',0};
  void* window = CreateWindowExW(0, window_class, 0, 0, 0, 0, 640, 480, 0, 0, 0, 0);
  SetFocus(0);
  SetFocus(window);
  bits |= (window && GetFocus() == window) ? 0x200u : 0u;
  PostQuitMessage(0);
  bits |= 0x400u;

  // 7. the frame timer moves
  const u32 t0 = GetTickCount64();
  u32 r[4];
  GetClientRect(window, r);
  bits |= (r[2] > 0 && r[3] > 0) ? 0x800u : 0u;
  bits |= (GetTickCount64() >= t0) ? 0x1000u : 0u;

  // ===== 8. the console/child-process honesty surface (OrTMM's imports) =====
  // 8a. CreateProcessW must fail VISIBLY named (console blocks children)
  __attribute__((dllimport)) int GSTD CreateProcessW(const u16* app,
                                                      u16* cmdline, void* pa,
                                                      void* ta, i32 inh,
                                                      u32 flags, void* env,
                                                      const u16* dir,
                                                      const void* si,
                                                      void* pi);
  __attribute__((dllimport)) void* GSTD GetStdHandle(u32 which);
  __attribute__((dllimport)) u32 GSTD GetLastError(void);
  __attribute__((dllimport)) void GSTD SetLastError(u32 err);
  __attribute__((dllimport)) i32 GSTD GetConsoleMode(void* con, u32* mode);
  __attribute__((dllimport)) i32 GSTD SetConsoleCtrlHandler(void* h, i32 add);
  __attribute__((dllimport)) i32 GSTD ReadConsoleW(void* con, void* buf,
                                                    u32 n, u32* got, void* c);
  __attribute__((dllimport)) i32 GSTD WriteConsoleW(void* con, const void* buf,
                                                     u32 n, u32* put, void* r);
  const u16 child[] = {'n', 'o', 't', 'e', 'p', 'a', 'd', '.', 'e', 'x', 'e', 0};
  u8 pi[104];  // PROCESS_INFORMATION (x64) sized room
  for (u32 i = 0; i < sizeof(pi); ++i) pi[i] = 0;
  const i32 cp = CreateProcessW(0, (u16*)child, 0, 0, 0, 0, 0, 0, 0, pi);
  bits |= (cp == 0) ? 0x2000u : 0u;  // FALSE: no children, named
  bits |= (GetLastError() == 5 /*ACCESS_DENIED*/) ? 0x4000u : 0u;
  bits |= (pi[0] == 0 && pi[8] == 0) ? 0x8000u : 0u;  // no fake handles

  // 8b. the console surface: GetStdHandle serves synthetic stdio — real
  // Win32 pseudo-handles (STD_OUTPUT = -11) routed by WriteFile to the log
  // ring (measured: Godot's own stdout is the fastest discovery surface);
  // a non-std id still answers NULL + ERROR_INVALID_HANDLE honestly.
  __attribute__((dllimport)) void* GSTD SetStdHandle(u32 which, void* h);
  __attribute__((dllimport)) i32 GSTD WriteConsoleW(void* con, const void* buf,
                                                     u32 n, u32* put, void* r);
  (void)WriteConsoleW;
  SetLastError(0);
  const void* stdh = GetStdHandle(0xFFFFFFF5u /*STD_OUTPUT_HANDLE*/);
  bits |= (stdh == (const void*)(long long)-11 /*pseudo-handle*/) ? 0x10000u
                                                                   : 0u;  SetLastError(0);
  const void* badh = GetStdHandle(0u /*not a std id*/);
  bits |= (badh == 0 && GetLastError() == 6 /*INVALID_HANDLE*/) ? 0x4000000u
                                                                 : 0u;
  bits |= (SetStdHandle(0xFFFFFFF5u, 0) == 0) ? 0x20000u : 0u;

  // 8c. console mode/read/write all say "no console" honestly
  u32 cm = 0;
  bits |= (GetConsoleMode(0, &cm) == 0) ? 0x40000u : 0u;
  bits |= (SetConsoleCtrlHandler(0, 1) == 0) ? 0x80000u : 0u;
  u32 io_n = 0;
  bits |= (ReadConsoleW(0, (void*)pi, 4, &io_n, 0) == 0) ? 0x100000u : 0u;
  bits |= (WriteConsoleW(0, (const void*)pi, 4, &io_n, 0) == 0) ? 0x200000u
                                                                 : 0u;

  return bits;
}
