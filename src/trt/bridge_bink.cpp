// tiramisu/src/trt/bridge_bink.cpp
// Bink 2 video (bink2w64.dll) bridge — expanded stubs.
//
// Bink is RAD Game Tools' proprietary video codec. Tiramisu does not
// implement it; these stubs let LoadLibrary/GetProcAddress succeed and
// return honest failure so games skip video instead of crashing on a
// missing DLL. Video playback remains unavailable.
//
// Bink error codes: 0 = no error. Non-zero = error. BinkOpen returns
// nullptr on failure, which games handle gracefully.
#include "bridge_bink.h"
#include "thunk.h"
#include "trt.h"

#include <stdio.h>

namespace {
namespace binklog { static void line(const char* s) { trt_core_log(s); } }
#define log_line(s_) binklog::line(s_)
}

extern "C" {

// Bink handle is opaque; we never allocate one.
void* TRT_GUEST_ABI bink_BinkOpen(const void* file, unsigned flags) {
    log_line("bink: BinkOpen -> NULL (Bink not implemented; video skipped)");
    return nullptr;
}

int TRT_GUEST_ABI bink_BinkClose(void* bnk) {
    return 0;
}

int TRT_GUEST_ABI bink_BinkDoFrame(void* bnk) {
    return 0;
}

int TRT_GUEST_ABI bink_BinkNextFrame(void* bnk) {
    return 0;
}

int TRT_GUEST_ABI bink_BinkWait(void* bnk) {
    return 0;
}

void TRT_GUEST_ABI bink_BinkGetRects(void* bnk, void* rects) {
}

int TRT_GUEST_ABI bink_BinkCopyToBuffer(void* bnk, void* buf, int pitch,
                                        int desth, int destx, int desty,
                                        unsigned flags) {
    return 0;
}

unsigned TRT_GUEST_ABI bink_BinkGetError(void) {
    return 0; // 0 = no error
}

// Additional Bink 2 functions
int TRT_GUEST_ABI bink_BinkSetVolume(void* bnk, int track, int volume) { return 0; }
int TRT_GUEST_ABI bink_BinkSetPan(void* bnk, int track, int pan) { return 0; }
int TRT_GUEST_ABI bink_BinkSetMixBins(void* bnk, int track, unsigned* bins, int nbins) { return 0; }
int TRT_GUEST_ABI bink_BinkSetMixBinVolumes(void* bnk, int track, unsigned* bins, int* volumes, int nbins) { return 0; }
int TRT_GUEST_ABI bink_BinkPause(void* bnk, int pause) { return 0; }
unsigned TRT_GUEST_ABI bink_BinkGetFrameNum(void* bnk) { return 0; }
unsigned TRT_GUEST_ABI bink_BinkGetKeyFrame(void* bnk) { return 0; }
int TRT_GUEST_ABI bink_BinkShouldSkip(void* bnk) { return 1; } // always skip
int TRT_GUEST_ABI bink_BinkGetVideoInfo(void* bnk, void* info) { return 0; }
int TRT_GUEST_ABI bink_BinkGetAudioInfo(void* bnk, void* info) { return 0; }
unsigned TRT_GUEST_ABI bink_BinkGetRealtime(void* bnk, void* rt) { return 0; }
int TRT_GUEST_ABI bink_BinkGoto(void* bnk, unsigned frame, int flags) { return 0; }
int TRT_GUEST_ABI bink_BinkGetSummary(void* bnk, void* summary) { return 0; }
void TRT_GUEST_ABI bink_BinkRegisterFrameBuffers(void* bnk, void* fb) {}
int TRT_GUEST_ABI bink_BinkSetSoundTrack(void* bnk, int track) { return 0; }
int TRT_GUEST_ABI bink_BinkSetVideoOnOff(void* bnk, int onoff) { return 0; }
int TRT_GUEST_ABI bink_BinkSetSoundOnOff(void* bnk, int onoff) { return 0; }

// Bink buffer management
void* TRT_GUEST_ABI bink_BinkAllocateFrameBuffers(void* bnk, void* fb) { return nullptr; }
void TRT_GUEST_ABI bink_BinkFreeFrameBuffers(void* bnk) {}

// Bink I/O
void* TRT_GUEST_ABI bink_BinkOpenDirect(const char* name, unsigned flags) { return nullptr; }

}  // extern "C"

extern "C" int trt_bridge_register_bink(void) {
    static const trt_named_fn k_fns[] = {
        {"BinkOpen", (void*)&bink_BinkOpen},
        {"BinkClose", (void*)&bink_BinkClose},
        {"BinkDoFrame", (void*)&bink_BinkDoFrame},
        {"BinkNextFrame", (void*)&bink_BinkNextFrame},
        {"BinkWait", (void*)&bink_BinkWait},
        {"BinkGetRects", (void*)&bink_BinkGetRects},
        {"BinkCopyToBuffer", (void*)&bink_BinkCopyToBuffer},
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
