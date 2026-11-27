// tiramisu/src/guest/guest_winmm.c
// Proves winmm!timeGetTime actually advances and timeBeginPeriod is
// accepted — the frame-timing clock games live on. Returns a bitmask:
//   0x01 timeBeginPeriod(1) accepted
//   0x02 clock nondecreasing
//   0x04 clock advances (within a busy-spin window)
//   0x08 waveOutGetNumDevs answers 1 (one real output device)
//   0x10 timeGetDevCaps returns sane min/max
//   0x20 mciSendStringW reports an unavailable device
//   0x40 mciSendStringA reports the same error
//   0x80 ANSI and UTF-16 error messages are bounded and terminated
//   0x100 unknown errors and zero-length buffers fail safely
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
typedef unsigned long long u64;

#ifdef _WIN32
#define GSTD __stdcall
#else
#define GSTD
#endif

int _fltused = 1;  // any float in the TU makes MSVC linkers want this

__attribute__((dllimport)) u32 GSTD timeGetTime(void);
__attribute__((dllimport)) u32 GSTD timeBeginPeriod(u32 ms);
__attribute__((dllimport)) u32 GSTD timeEndPeriod(u32 ms);
__attribute__((dllimport)) u32 GSTD timeGetDevCaps(void* caps, u32 size);
__attribute__((dllimport)) u32 GSTD waveOutGetNumDevs(void);
__attribute__((dllimport)) u32 GSTD mciSendStringW(const u16* cmd, u16* ret,
                                                    u32 ret_len, void* hwnd);

__attribute__((dllimport)) u32 GSTD mciSendStringA(const char* cmd, char* ret,
                                                    u32 ret_len, void* hwnd);
__attribute__((dllimport)) u32 GSTD mciGetErrorStringA(u32 error, char* out, u32 cap);
__attribute__((dllimport)) u32 GSTD mciGetErrorStringW(u32 error, u16* out, u32 cap);

u32 GSTD guest_winmm_main(u64 a, u64 b, u64 c) {
  (void)a; (void)b; (void)c;
  u32 bits = 0;

  // 1. raise timer resolution
  if (timeBeginPeriod(1) == 0) bits |= 0x01u;

  // 2. clock nondecreasing across a busy window, and actually advances
  const u32 t0 = timeGetTime();
  u32 t1 = t0;
  for (volatile int i = 0; i < 3000000; ++i) {}
  t1 = timeGetTime();
  if (t1 >= t0) bits |= 0x02u;
  if (t1 > t0) bits |= 0x04u;

  // 3. legacy device probes answer without crashing
  if (waveOutGetNumDevs() == 1) bits |= 0x08u;

  // 4. TIMECAPS sane
  u32 caps[2] = {0, 0};
  if (timeGetDevCaps(caps, (u32)sizeof(caps)) == 0 && caps[0] >= 1 &&
      caps[1] >= caps[0])
    bits |= 0x10u;

  // 5. MCI fails visibly with the "no such device" code, no crash
  const u16 cmd[] = { 'o','p','e','n',' ','p','a','u','s','e','.','w','a','v',
                      ' ','t','y','p','e',' ','w','a','v','e','a','u','d','i',
                      'o', 0 };
  u16 ret[32];
  const u32 wide_error = mciSendStringW(cmd, ret, 32, 0);
  if (wide_error == 263) bits |= 0x20u;
  const u32 ansi_error = mciSendStringA("open pause.wav", 0, 0, 0);
  if (ansi_error == 263) bits |= 0x40u;
  char error_a[4] = {'~', '~', '~', '!'};
  u16 error_w[4] = {'~', '~', '~', '!'};
  if (mciGetErrorStringA(ansi_error, error_a, 3) &&
      mciGetErrorStringW(wide_error, error_w, 3) &&
      error_a[0] == error_w[0] && error_a[1] == error_w[1] &&
      error_a[0] != '~' && error_a[2] == 0 && error_w[2] == 0 &&
      error_a[3] == '!' && error_w[3] == '!') bits |= 0x80u;
  char unknown = '~';
  if (!mciGetErrorStringA(0xffffffffu, &unknown, 1) && unknown == 0 &&
      !mciGetErrorStringW(263, error_w, 0) && error_w[0] != 0)
    bits |= 0x100u;

  timeEndPeriod(1);
  return bits;
}
