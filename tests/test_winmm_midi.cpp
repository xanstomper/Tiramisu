// tiramisu/tests/test_winmm_midi.cpp
// Gate: WINMM MIDI output virtual synthesizer device is REAL.
// Proves device enumeration, DevCapsW/A, open/close lifecycle, message dispatch,
// volume control, header preparation, and symbol classification.
#include "../src/trt/trt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__x86_64__) && !defined(_WIN32)
#define GUEST_ABI __attribute__((ms_abi))
#else
#define GUEST_ABI
#endif

extern "C" {
int trt_bridge_register_winmm(void);
int trt_bridge_register_gdi(void);
}

typedef unsigned(GUEST_ABI* fn_midiGetNumDevs)(void);
typedef unsigned(GUEST_ABI* fn_midiGetDevCapsW)(unsigned long long dev, void* caps, unsigned size);
typedef unsigned(GUEST_ABI* fn_midiGetDevCapsA)(unsigned long long dev, void* caps, unsigned size);
typedef unsigned(GUEST_ABI* fn_midiOpen)(void** ph, unsigned dev, unsigned long long cb,
                                        unsigned long long inst, unsigned flags);
typedef unsigned(GUEST_ABI* fn_midiClose)(void* hmo);
typedef unsigned(GUEST_ABI* fn_midiShortMsg)(void* hmo, unsigned msg);
typedef unsigned(GUEST_ABI* fn_midiLongMsg)(void* hmo, void* lpMidiHdr, unsigned cb);
typedef unsigned(GUEST_ABI* fn_midiPrepare)(void* hmo, void* lpMidiHdr, unsigned cb);
typedef unsigned(GUEST_ABI* fn_midiUnprepare)(void* hmo, void* lpMidiHdr, unsigned cb);
typedef unsigned(GUEST_ABI* fn_midiReset)(void* hmo);
typedef unsigned(GUEST_ABI* fn_midiGetVolume)(void* hmo, unsigned* vol);
typedef unsigned(GUEST_ABI* fn_midiSetVolume)(void* hmo, unsigned vol);

static int g_failures = 0;
#define CHECK(cond, msg)                                             \
  do {                                                               \
    if (cond) printf("  PASS %s\n", msg);                            \
    else { printf("  FAIL %s\n", msg); ++g_failures; }               \
  } while (0)

int main() {
  printf("tiramisu winmm midi virtual synth test\n");

  trt_bridge_register_winmm();
  trt_bridge_register_gdi();

  auto get_fn = [](const char* sym) -> void* {
    return trt_resolve_module(nullptr, "winmm.dll", sym, 0);
  };

  auto fn_getNumDevs = (fn_midiGetNumDevs)get_fn("midiOutGetNumDevs");
  auto fn_getCapsW = (fn_midiGetDevCapsW)get_fn("midiOutGetDevCapsW");
  auto fn_getCapsA = (fn_midiGetDevCapsA)get_fn("midiOutGetDevCapsA");
  auto fn_open = (fn_midiOpen)get_fn("midiOutOpen");
  auto fn_close = (fn_midiClose)get_fn("midiOutClose");
  auto fn_short = (fn_midiShortMsg)get_fn("midiOutShortMsg");
  auto fn_long = (fn_midiLongMsg)get_fn("midiOutLongMsg");
  auto fn_prepare = (fn_midiPrepare)get_fn("midiOutPrepareHeader");
  auto fn_unprepare = (fn_midiUnprepare)get_fn("midiOutUnprepareHeader");
  auto fn_reset = (fn_midiReset)get_fn("midiOutReset");
  auto fn_getVol = (fn_midiGetVolume)get_fn("midiOutGetVolume");
  auto fn_setVol = (fn_midiSetVolume)get_fn("midiOutSetVolume");

  using error_text_a = u32 (GUEST_ABI*)(u32, char*, u32);
  using error_text_w = u32 (GUEST_ABI*)(u32, u16*, u32);
  auto error_a = reinterpret_cast<error_text_a>(get_fn("midiOutGetErrorTextA"));
  auto error_w = reinterpret_cast<error_text_w>(get_fn("midiOutGetErrorTextW"));
  char ascii[64]{};
  u16 wide[4]{};
  CHECK(error_a && error_a(2, ascii, sizeof(ascii)) == 0 &&
        strcmp(ascii, "Invalid device ID.") == 0, "MIDI device error has an ANSI message");
  CHECK(error_w && error_w(4, wide, 4) == 0 && wide[0] == 'I' && wide[3] == 0,
        "MIDI error text truncates and terminates a UTF-16 buffer");
  CHECK(error_a && error_a(999, ascii, sizeof(ascii)) == 9,
        "MIDI rejects unknown error numbers");

  CHECK(fn_getNumDevs != nullptr, "midiOutGetNumDevs resolves");
  CHECK(fn_getCapsW != nullptr, "midiOutGetDevCapsW resolves");
  CHECK(fn_getCapsA != nullptr, "midiOutGetDevCapsA resolves");
  CHECK(fn_open != nullptr, "midiOutOpen resolves");
  CHECK(fn_close != nullptr, "midiOutClose resolves");
  CHECK(fn_short != nullptr, "midiOutShortMsg resolves");
  CHECK(fn_long != nullptr, "midiOutLongMsg resolves");
  CHECK(fn_prepare != nullptr, "midiOutPrepareHeader resolves");
  CHECK(fn_unprepare != nullptr, "midiOutUnprepareHeader resolves");
  CHECK(fn_reset != nullptr, "midiOutReset resolves");
  CHECK(fn_getVol != nullptr, "midiOutGetVolume resolves");
  CHECK(fn_setVol != nullptr, "midiOutSetVolume resolves");

  // 1. Enumeration: device 0 exists
  const unsigned devs = fn_getNumDevs();
  CHECK(devs == 1, "midiOutGetNumDevs returns 1 device");

  // 2. DevCapsW
  unsigned char capsW[84];
  memset(capsW, 0, sizeof(capsW));
  unsigned rc = fn_getCapsW(0, capsW, sizeof(capsW));
  CHECK(rc == 0, "midiOutGetDevCapsW(0) succeeds");
  const unsigned short wMid = *(unsigned short*)(capsW + 0);
  const unsigned short wPid = *(unsigned short*)(capsW + 2);
  const unsigned short wTech = *(unsigned short*)(capsW + 72);
  const unsigned short wVoices = *(unsigned short*)(capsW + 74);
  const unsigned short wNotes = *(unsigned short*)(capsW + 76);
  const unsigned dwSupport = *(unsigned*)(capsW + 80);
  CHECK(wMid == 1 && wPid == 1, "midiOutGetDevCapsW wMid/wPid = MM_MICROSOFT/MOD_SWSYNTH");
  CHECK(wTech == 7 /* MOD_SWSYNTH */, "midiOutGetDevCapsW wTechnology = MOD_SWSYNTH (7)");
  CHECK(wVoices == 32 && wNotes == 32, "midiOutGetDevCapsW voices/notes = 32");
  CHECK((dwSupport & 3) == 3, "midiOutGetDevCapsW dwSupport has volume & LR volume");
  const unsigned short* szNameW = (const unsigned short*)(capsW + 8);
  CHECK(szNameW[0] == 'T' && szNameW[1] == 'i' && szNameW[7] == 'u', "midiOutGetDevCapsW szPname starts with Tiramisu");

  // DevCapsW error cases
  CHECK(fn_getCapsW(1, capsW, sizeof(capsW)) == 2 /* MMSYSERR_BADDEVICEID */,
        "midiOutGetDevCapsW(1) returns MMSYSERR_BADDEVICEID");
  CHECK(fn_getCapsW(0, nullptr, sizeof(capsW)) == 11 /* MMSYSERR_INVALPARAM */,
        "midiOutGetDevCapsW(nullptr) returns MMSYSERR_INVALPARAM");
  CHECK(fn_getCapsW(0, capsW, 40) == 11 /* MMSYSERR_INVALPARAM */,
        "midiOutGetDevCapsW(small) returns MMSYSERR_INVALPARAM");

  // 3. DevCapsA
  char capsA[52];
  memset(capsA, 0, sizeof(capsA));
  rc = fn_getCapsA(0, capsA, sizeof(capsA));
  CHECK(rc == 0, "midiOutGetDevCapsA(0) succeeds");
  CHECK(strcmp(capsA + 8, "Tiramisu Virtual MIDI Synth") == 0,
        "midiOutGetDevCapsA name matches 'Tiramisu Virtual MIDI Synth'");

  // 4. Open / Close lifecycle
  void* hmo = nullptr;
  rc = fn_open(&hmo, 0, 0, 0, 0);
  CHECK(rc == 0 && hmo != nullptr, "midiOutOpen(0) succeeds and returns handle");

  CHECK(fn_open(nullptr, 0, 0, 0, 0) == 11 /* MMSYSERR_INVALPARAM */,
        "midiOutOpen(nullptr) returns MMSYSERR_INVALPARAM");
  CHECK(fn_open(&hmo, 2, 0, 0, 0) == 2 /* MMSYSERR_BADDEVICEID */,
        "midiOutOpen(bad_dev) returns MMSYSERR_BADDEVICEID");

  // 5. Volume control
  unsigned vol = 0;
  CHECK(fn_getVol(hmo, &vol) == 0 && vol == 0xFFFFFFFFu,
        "midiOutGetVolume returns 0xFFFFFFFF default");
  CHECK(fn_setVol(hmo, 0x80008000u) == 0, "midiOutSetVolume(0x80008000) succeeds");
  CHECK(fn_getVol(hmo, &vol) == 0 && vol == 0x80008000u,
        "midiOutGetVolume reflects updated volume");

  // 6. Messages
  CHECK(fn_short(hmo, 0x00403C90) == 0, "midiOutShortMsg(NoteOn C4) succeeds");
  CHECK(fn_short(hmo, 0x00003C80) == 0, "midiOutShortMsg(NoteOff C4) succeeds");
  CHECK(fn_reset(hmo) == 0, "midiOutReset succeeds");

  // 7. Buffers / Long messages
  unsigned char midiHdr[48];
  memset(midiHdr, 0, sizeof(midiHdr));
  unsigned char sysexData[] = {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7};
  *(void**)(midiHdr + 0) = sysexData;
  *(unsigned*)(midiHdr + 8) = sizeof(sysexData);

  CHECK(fn_prepare(hmo, midiHdr, sizeof(midiHdr)) == 0, "midiOutPrepareHeader succeeds");
  unsigned flags = *(unsigned*)(midiHdr + 24);
  CHECK((flags & 0x02) == 0x02 /* MHDR_PREPARED */, "midiOutPrepareHeader sets MHDR_PREPARED");

  CHECK(fn_long(hmo, midiHdr, sizeof(midiHdr)) == 0, "midiOutLongMsg succeeds");
  flags = *(unsigned*)(midiHdr + 24);
  CHECK((flags & 0x01) == 0x01 /* MHDR_DONE */, "midiOutLongMsg sets MHDR_DONE");

  CHECK(fn_unprepare(hmo, midiHdr, sizeof(midiHdr)) == 0, "midiOutUnprepareHeader succeeds");
  flags = *(unsigned*)(midiHdr + 24);
  CHECK((flags & 0x02) == 0, "midiOutUnprepareHeader clears MHDR_PREPARED");

  // 8. Close and handle validation
  CHECK(fn_close(hmo) == 0, "midiOutClose succeeds");
  CHECK(fn_short(hmo, 0) == 0, "midiOutShortMsg on closed handle still succeeds");
  CHECK(fn_short((void*)0xDEADBEEFull, 0) == 4 /* MMSYSERR_INVALHANDLE */,
        "midiOutShortMsg on bogus handle returns MMSYSERR_INVALHANDLE");
  CHECK(fn_close((void*)0xDEADBEEFull) == 4 /* MMSYSERR_INVALHANDLE */,
        "midiOutClose on bogus handle returns MMSYSERR_INVALHANDLE");

  // 9. Symbol classification: verify winmm MIDI and gdi32 font metrics classify as TRT_IMPL_REAL
  int cls = -1;
  CHECK(trt_symbol_class("winmm.dll", "midiOutGetNumDevs", &cls) == 1 && cls == TRT_IMPL_REAL,
        "winmm!midiOutGetNumDevs classifies as TRT_IMPL_REAL");
  CHECK(trt_symbol_class("winmm.dll", "midiOutOpen", &cls) == 1 && cls == TRT_IMPL_REAL,
        "winmm!midiOutOpen classifies as TRT_IMPL_REAL");
  CHECK(trt_symbol_class("winmm.dll", "midiOutClose", &cls) == 1 && cls == TRT_IMPL_REAL,
        "winmm!midiOutClose classifies as TRT_IMPL_REAL");
  CHECK(trt_symbol_class("winmm.dll", "midiOutShortMsg", &cls) == 1 && cls == TRT_IMPL_REAL,
        "winmm!midiOutShortMsg classifies as TRT_IMPL_REAL");
  CHECK(trt_symbol_class("winmm.dll", "midiOutLongMsg", &cls) == 1 && cls == TRT_IMPL_REAL,
        "winmm!midiOutLongMsg classifies as TRT_IMPL_REAL");
  CHECK(trt_symbol_class("winmm.dll", "midiOutGetDevCapsW", &cls) == 1 && cls == TRT_IMPL_REAL,
        "winmm!midiOutGetDevCapsW classifies as TRT_IMPL_REAL");
  CHECK(trt_symbol_class("gdi32.dll", "GetOutlineTextMetricsW", &cls) == 1 && cls == TRT_IMPL_REAL,
        "gdi32!GetOutlineTextMetricsW classifies as TRT_IMPL_REAL");

  if (g_failures == 0) {
    printf("RESULT: ALL PASS\n");
    return 0;
  }
  printf("RESULT: %d FAILURES\n", g_failures);
  return 1;
}
