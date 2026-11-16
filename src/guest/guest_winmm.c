// tiramisu/src/guest/guest_winmm.c
// Proves winmm!timeGetTime actually advances and timeBeginPeriod is
// accepted — the frame-timing clock games live on. Returns a bitmask:
//   0x01 timeBeginPeriod(1) accepted
//   0x02 clock nondecreasing
//   0x04 clock advances (within a busy-spin window)
//   0x08 waveOutGetNumDevs answers 0 without crashing
//   0x10 timeGetDevCaps returns sane min/max
//   0x20 mciSendStringW fails visibly (263), no crash
//   0x40 timeSetEvent oneshot fired the guest callback (host thread)
//   0x80 timeKillEvent stopped the periodic timer (callback count froze)
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

// ---- multimedia-timer callback state (host thread writes via trt_call_guest)
static volatile u32 g_fire_count = 0;
static volatile u32 g_last_id = 0;

void GSTD mm_timer_cb(u32 id, u32 msg, u64 user, u64 a1, u64 a2) {
  (void)msg; (void)a1; (void)a2;
  g_last_id = id;
  ++g_fire_count;  // racy increment is fine: the checks only need ordering
}

__attribute__((dllimport)) u32 GSTD timeGetTime(void);
__attribute__((dllimport)) u32 GSTD timeBeginPeriod(u32 ms);
__attribute__((dllimport)) u32 GSTD timeEndPeriod(u32 ms);
__attribute__((dllimport)) u32 GSTD timeGetDevCaps(void* caps, u32 size);
__attribute__((dllimport)) u32 GSTD timeSetEvent(u32 period, u32 res,
                                                 void* cb, u64 user, u32 flags);
__attribute__((dllimport)) u32 GSTD timeKillEvent(u32 id);
__attribute__((dllimport)) u32 GSTD waveOutGetNumDevs(void);
__attribute__((dllimport)) u32 GSTD mciSendStringW(const u16* cmd, u16* ret,
                                                    u32 ret_len, void* hwnd);

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
  if (waveOutGetNumDevs() == 0) bits |= 0x08u;

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
  if (mciSendStringW(cmd, ret, 32, 0) == 263) bits |= 0x20u;

  // 6. multimedia timers: one-shot fires the guest callback from a host
  //    thread (through the same trt_call_guest gate CreateThread uses —
  //    this is the exact contract OpenTTD's scheduler lives on).
  const u32 one = timeSetEvent(20, 10, (void*)&mm_timer_cb, 0x1234u, 0x0000u);
  if (one) {
    for (volatile int i = 0; i < 5000000 && g_fire_count == 0; ++i) {}
    if (g_fire_count >= 1) bits |= 0x40u;
    timeKillEvent(one);
  }

  // 7. periodic timer: count advances while live, freezes after kill
  g_fire_count = 0;
  const u32 per = timeSetEvent(30, 10, (void*)&mm_timer_cb, 0x5678u, 0x0001u);
  if (per) {
    for (volatile int i = 0; i < 3000000; ++i) {}
    const u32 during = g_fire_count;
    timeKillEvent(per);
    const u32 after = g_fire_count;
    for (volatile int i = 0; i < 5000000; ++i) {}
    if (during >= 1 && g_fire_count - after <= 1) bits |= 0x80u;
  }

  timeEndPeriod(1);
  return bits;
}
