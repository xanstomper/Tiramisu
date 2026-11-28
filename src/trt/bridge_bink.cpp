// tiramisu/src/trt/bridge_bink.cpp
// Bink / Bink 2 video (bink2w64.dll, binkw64.dll, bink2w32.dll, binkw32.dll)
// bridge with per-handle frame progression & anti-softlock cutscene handling.
//
// Bink is RAD Game Tools' proprietary video codec. When a game does not ship
// a native Bink DLL (or routes through the built-in bridge), each opened handle
// tracks a clean 1-frame lifecycle so cutscene loops (`while (FrameNum < Frames)`
// or `if (BinkWait(b) == 0) BinkNextFrame(b)`) advance immediately and never
// soft-lock, while BinkCopyToBuffer clears the target buffer cleanly to black.
#include "bridge_bink.h"
#include "thunk.h"
#include "trt.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_set>

namespace {
namespace binklog { static void line(const char* s) { trt_core_log(s); } }
#define log_line(s_) binklog::line(s_)

struct BinkHeader {
  unsigned int Width;
  unsigned int Height;
  unsigned int Frames;
  unsigned int FrameNum;
  unsigned int LastFrameNum;
  unsigned int FrameRate;
  unsigned int FrameRateDiv;
  unsigned int ReadError;
  unsigned int OpenFlags;
  unsigned int BinkType;
  unsigned int Size;
  unsigned int FrameSize;
  unsigned int SndSize;
  unsigned int NumTracks;
};

struct BinkInstance {
  BinkHeader hdr;  // MUST stay at offset 0 (guests read fields directly via BINK*)
  unsigned int magic;
  int paused;
};

constexpr unsigned int k_bink_magic = 0x42494E4Bu;  // 'BINK'
std::mutex g_bink_mu;
std::unordered_set<BinkInstance*> g_bink_live;

BinkInstance* alloc_bink_instance(unsigned flags) {
  auto* inst = static_cast<BinkInstance*>(calloc(1, sizeof(BinkInstance)));
  if (!inst) return nullptr;
  inst->hdr.Width = 1920;
  inst->hdr.Height = 1080;
  inst->hdr.Frames = 1;
  inst->hdr.FrameNum = 1;
  inst->hdr.LastFrameNum = 1;
  inst->hdr.FrameRate = 30;
  inst->hdr.FrameRateDiv = 1;
  inst->hdr.OpenFlags = flags;
  inst->magic = k_bink_magic;
  inst->paused = 0;
  std::lock_guard<std::mutex> lk(g_bink_mu);
  g_bink_live.insert(inst);
  return inst;
}

BinkInstance* as_bink(void* p) {
  if (!p) return nullptr;
  std::lock_guard<std::mutex> lk(g_bink_mu);
  auto* inst = static_cast<BinkInstance*>(p);
  if (g_bink_live.find(inst) == g_bink_live.end() || inst->magic != k_bink_magic)
    return nullptr;
  return inst;
}

}  // namespace

extern "C" {

void* TRT_GUEST_ABI bink_BinkOpen(const void* file, unsigned flags) {
  (void)file;
  log_line("bink: BinkOpen -> 1-frame progression handle (cutscene skip, anti-softlock)");
  return alloc_bink_instance(flags);
}

void* TRT_GUEST_ABI bink_BinkOpenWithOptions(const void* file, const void* opts, unsigned flags) {
  (void)opts;
  return bink_BinkOpen(file, flags);
}

void* TRT_GUEST_ABI bink_BinkOpenDirect(const char* name, unsigned flags) {
  (void)name;
  log_line("bink: BinkOpenDirect -> 1-frame progression handle (cutscene skip, anti-softlock)");
  return alloc_bink_instance(flags);
}

int TRT_GUEST_ABI bink_BinkClose(void* bnk) {
  if (!bnk) return 0;
  BinkInstance* inst = nullptr;
  {
    std::lock_guard<std::mutex> lk(g_bink_mu);
    auto* cand = static_cast<BinkInstance*>(bnk);
    auto it = g_bink_live.find(cand);
    if (it != g_bink_live.end()) {
      inst = cand;
      inst->magic = 0;
      g_bink_live.erase(it);
    }
  }
  free(inst);
  return 0;
}

int TRT_GUEST_ABI bink_BinkDoFrame(void* bnk) {
  if (auto* inst = as_bink(bnk)) {
    inst->hdr.LastFrameNum = inst->hdr.FrameNum;
  }
  return 0;
}

int TRT_GUEST_ABI bink_BinkNextFrame(void* bnk) {
  if (auto* inst = as_bink(bnk)) {
    if (!inst->paused && inst->hdr.FrameNum < inst->hdr.Frames) {
      ++inst->hdr.FrameNum;
    }
  }
  return 0;
}

int TRT_GUEST_ABI bink_BinkWait(void* bnk) {
  (void)bnk;
  return 0;  // 0 = ready for next frame immediately (never stalls cutscene timer)
}

void TRT_GUEST_ABI bink_BinkGetRects(void* bnk, void* rects) {
  (void)bnk; (void)rects;
}

int TRT_GUEST_ABI bink_BinkCopyToBuffer(void* bnk, void* buf, int pitch,
                                        int desth, int destx, int desty,
                                        unsigned flags) {
  (void)bnk; (void)destx; (void)desty; (void)flags;
  if (buf && pitch > 0 && desth > 0) {
    const size_t total = static_cast<size_t>(pitch) * static_cast<size_t>(desth);
    if (total <= (64u << 20)) memset(buf, 0, total);
  }
  return 0;
}

int TRT_GUEST_ABI bink_BinkCopyToBufferRect(void* bnk, void* buf, int pitch,
                                            int desth, int destx, int desty,
                                            int srcx, int srcy, int srcw, int srch,
                                            unsigned flags) {
  (void)srcx; (void)srcy; (void)srcw; (void)srch;
  return bink_BinkCopyToBuffer(bnk, buf, pitch, desth, destx, desty, flags);
}

unsigned TRT_GUEST_ABI bink_BinkGetError(void) { return 0; }
int TRT_GUEST_ABI bink_BinkSetVolume(void* bnk, int track, int volume) { (void)bnk; (void)track; (void)volume; return 0; }
int TRT_GUEST_ABI bink_BinkSetPan(void* bnk, int track, int pan) { (void)bnk; (void)track; (void)pan; return 0; }
int TRT_GUEST_ABI bink_BinkSetMixBins(void* bnk, int track, unsigned* bins, int nbins) { (void)bnk; (void)track; (void)bins; (void)nbins; return 0; }
int TRT_GUEST_ABI bink_BinkSetMixBinVolumes(void* bnk, int track, unsigned* bins, int* volumes, int nbins) { (void)bnk; (void)track; (void)bins; (void)volumes; (void)nbins; return 0; }

int TRT_GUEST_ABI bink_BinkPause(void* bnk, int pause) {
  if (auto* inst = as_bink(bnk)) {
    inst->paused = pause ? 1 : 0;
    return inst->paused;
  }
  return 0;
}

unsigned TRT_GUEST_ABI bink_BinkGetFrameNum(void* bnk) {
  if (auto* inst = as_bink(bnk)) return inst->hdr.FrameNum;
  return 1;
}

unsigned TRT_GUEST_ABI bink_BinkGetKeyFrame(void* bnk) { (void)bnk; return 1; }
int TRT_GUEST_ABI bink_BinkShouldSkip(void* bnk) { (void)bnk; return 1; }

int TRT_GUEST_ABI bink_BinkGetVideoInfo(void* bnk, void* info) {
  (void)bnk;
  if (info) memset(info, 0, 64);
  return 0;
}

int TRT_GUEST_ABI bink_BinkGetAudioInfo(void* bnk, void* info) {
  (void)bnk;
  if (info) memset(info, 0, 64);
  return 0;
}

unsigned TRT_GUEST_ABI bink_BinkGetRealtime(void* bnk, void* rt) {
  (void)bnk;
  if (rt) memset(rt, 0, 64);
  return 0;
}

int TRT_GUEST_ABI bink_BinkGoto(void* bnk, unsigned frame, int flags) {
  (void)flags;
  if (auto* inst = as_bink(bnk)) {
    inst->hdr.FrameNum = (frame <= inst->hdr.Frames) ? frame : inst->hdr.Frames;
  }
  return 0;
}

int TRT_GUEST_ABI bink_BinkGetSummary(void* bnk, void* summary) {
  (void)bnk;
  if (summary) memset(summary, 0, 96);
  return 0;
}

void TRT_GUEST_ABI bink_BinkRegisterFrameBuffers(void* bnk, void* fb) { (void)bnk; (void)fb; }
int TRT_GUEST_ABI bink_BinkSetSoundTrack(void* bnk, int track) { (void)bnk; (void)track; return 0; }
int TRT_GUEST_ABI bink_BinkSetVideoOnOff(void* bnk, int onoff) { (void)bnk; (void)onoff; return 0; }
int TRT_GUEST_ABI bink_BinkSetSoundOnOff(void* bnk, int onoff) { (void)bnk; (void)onoff; return 0; }
void* TRT_GUEST_ABI bink_BinkAllocateFrameBuffers(void* bnk, void* fb) { (void)bnk; (void)fb; return nullptr; }
void TRT_GUEST_ABI bink_BinkFreeFrameBuffers(void* bnk) { (void)bnk; }
int TRT_GUEST_ABI bink_BinkSetSoundSystem(void* open, void* param) { (void)open; (void)param; return 1; }
void* TRT_GUEST_ABI bink_BinkOpenDirectSound(void* param) { (void)param; return nullptr; }
void* TRT_GUEST_ABI bink_BinkOpenXAudio2(void* param) { (void)param; return nullptr; }
int TRT_GUEST_ABI bink_BinkControlBackgroundIO(void* bnk, unsigned control) { (void)bnk; (void)control; return 0; }
void TRT_GUEST_ABI bink_BinkSetFrameRate(unsigned rate, unsigned div) { (void)rate; (void)div; }
void* TRT_GUEST_ABI bink_BinkBufferOpen(void* wnd, unsigned w, unsigned h, unsigned flags) {
  (void)wnd; (void)w; (void)h; (void)flags;
  return nullptr;
}
void TRT_GUEST_ABI bink_BinkBufferClose(void* buf) { (void)buf; }
int TRT_GUEST_ABI bink_BinkBufferLock(void* buf) { (void)buf; return 0; }
int TRT_GUEST_ABI bink_BinkBufferUnlock(void* buf) { (void)buf; return 0; }

}  // extern "C"

extern "C" int trt_bridge_register_bink(void) {
  static const trt_named_fn k_fns[] = {
      {"BinkOpen", (void*)&bink_BinkOpen},
      {"BinkOpenWithOptions", (void*)&bink_BinkOpenWithOptions},
      {"BinkClose", (void*)&bink_BinkClose},
      {"BinkDoFrame", (void*)&bink_BinkDoFrame},
      {"BinkNextFrame", (void*)&bink_BinkNextFrame},
      {"BinkWait", (void*)&bink_BinkWait},
      {"BinkGetRects", (void*)&bink_BinkGetRects},
      {"BinkCopyToBuffer", (void*)&bink_BinkCopyToBuffer},
      {"BinkCopyToBufferRect", (void*)&bink_BinkCopyToBufferRect},
      {"BinkGetError", (void*)&bink_BinkGetError},
      {"BinkSetVolume", (void*)&bink_BinkSetVolume},
      {"BinkSetPan", (void*)&bink_BinkSetPan},
      {"BinkSetMixBins", (void*)&bink_BinkSetMixBins},
      {"BinkSetMixBinVolumes", (void*)&bink_BinkSetMixBinVolumes},
      {"BinkPause", (void*)&bink_BinkPause},
      {"BinkGetFrameNum", (void*)&bink_BinkGetFrameNum},
      {"BinkGetKeyFrame", (void*)&bink_BinkGetKeyFrame},
      {"BinkShouldSkip", (void*)&bink_BinkShouldSkip},
      {"BinkGetVideoInfo", (void*)&bink_BinkGetVideoInfo},
      {"BinkGetAudioInfo", (void*)&bink_BinkGetAudioInfo},
      {"BinkGetRealtime", (void*)&bink_BinkGetRealtime},
      {"BinkGoto", (void*)&bink_BinkGoto},
      {"BinkGetSummary", (void*)&bink_BinkGetSummary},
      {"BinkRegisterFrameBuffers", (void*)&bink_BinkRegisterFrameBuffers},
      {"BinkSetSoundTrack", (void*)&bink_BinkSetSoundTrack},
      {"BinkSetVideoOnOff", (void*)&bink_BinkSetVideoOnOff},
      {"BinkSetSoundOnOff", (void*)&bink_BinkSetSoundOnOff},
      {"BinkAllocateFrameBuffers", (void*)&bink_BinkAllocateFrameBuffers},
      {"BinkFreeFrameBuffers", (void*)&bink_BinkFreeFrameBuffers},
      {"BinkOpenDirect", (void*)&bink_BinkOpenDirect},
      {"BinkSetSoundSystem", (void*)&bink_BinkSetSoundSystem},
      {"BinkOpenDirectSound", (void*)&bink_BinkOpenDirectSound},
      {"BinkOpenXAudio2", (void*)&bink_BinkOpenXAudio2},
      {"BinkControlBackgroundIO", (void*)&bink_BinkControlBackgroundIO},
      {"BinkSetFrameRate", (void*)&bink_BinkSetFrameRate},
      {"BinkBufferOpen", (void*)&bink_BinkBufferOpen},
      {"BinkBufferClose", (void*)&bink_BinkBufferClose},
      {"BinkBufferLock", (void*)&bink_BinkBufferLock},
      {"BinkBufferUnlock", (void*)&bink_BinkBufferUnlock},
  };
  trt_module_register("bink2w64.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  trt_module_register("binkw64.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  trt_module_register("bink2w32.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  trt_module_register("binkw32.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  return 0;
}
