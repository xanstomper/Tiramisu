// tiramisu/src/trt/bridge_wwise.cpp
// Wwise (AK::SoundEngine) bridge — expanded stubs.
//
// Wwise is Audiokinetic's audio middleware used by many modern games.
// Tiramisu does not implement full Wwise audio decode; these stubs let
// LoadLibrary/GetProcAddress succeed and return AK_Success (0) so games
// proceed past audio init. Audio playback remains unavailable for
// compressed Wwise payloads (Vorbis/ADPCM bank data). Bank file loading
// is logged for diagnostics.
#include "bridge_wwise.h"
#include "thunk.h"
#include "trt.h"

#include <stdio.h>
#include <string.h>

namespace {
namespace wwiselog { static void line(const char* s) { trt_core_log(s); } }
#define log_line(s_) wwiselog::line(s_)
}

extern "C" {

static const int AK_SUCCESS = 0;
static int s_bank_count = 0;

// ---- Lifecycle ----
int TRT_GUEST_ABI wwise_AK_SoundEngine_Init(void* settings, void* platform) {
    log_line("wwise: AK::SoundEngine::Init -> AK_Success (stub)");
    return AK_SUCCESS;
}
int TRT_GUEST_ABI wwise_AK_SoundEngine_Term(void) {
    log_line("wwise: AK::SoundEngine::Term (no-op)");
    return AK_SUCCESS;
}
int TRT_GUEST_ABI wwise_AK_SoundEngine_RenderAudio(void) { return AK_SUCCESS; }
unsigned TRT_GUEST_ABI wwise_AK_SoundEngine_GetDefaultInitSettings(void* s) { return 0; }
unsigned TRT_GUEST_ABI wwise_AK_SoundEngine_GetDefaultPlatformInitSettings(void* s) { return 0; }

// ---- Memory Manager ----
int TRT_GUEST_ABI wwise_AK_MemoryMgr_Init(void* s) { return AK_SUCCESS; }
int TRT_GUEST_ABI wwise_AK_MemoryMgr_Term(void) { return AK_SUCCESS; }
void* TRT_GUEST_ABI wwise_AK_MemoryMgr_Malloc(unsigned size) { return nullptr; }
void TRT_GUEST_ABI wwise_AK_MemoryMgr_Free(void* p) {}

// ---- Stream Manager ----
int TRT_GUEST_ABI wwise_AK_StreamMgr_Create(void* s) { return AK_SUCCESS; }
void TRT_GUEST_ABI wwise_AK_StreamMgr_Destroy(void) {}
int TRT_GUEST_ABI wwise_AK_StreamMgr_SetFileLocationResolver(void* r) { return AK_SUCCESS; }

// ---- Music Engine ----
int TRT_GUEST_ABI wwise_AK_MusicEngine_Init(void* s) { return AK_SUCCESS; }
int TRT_GUEST_ABI wwise_AK_MusicEngine_Term(void) { return AK_SUCCESS; }

// ---- Bank Loading ----
unsigned TRT_GUEST_ABI wwise_AK_SoundEngine_LoadBank(const char* bank_path, unsigned flags, unsigned* bank_id) {
    char buf[512];
    snprintf(buf, sizeof(buf), "wwise: LoadBank('%s') -> stub (bank not loaded, AK_Success)", bank_path ? bank_path : "(null)");
    log_line(buf);
    if (bank_id) *bank_id = ++s_bank_count;
    return AK_SUCCESS;
}
unsigned TRT_GUEST_ABI wwise_AK_SoundEngine_LoadBankMemory(const void* data, unsigned size, unsigned flags, unsigned* bank_id) {
    char buf[256];
    snprintf(buf, sizeof(buf), "wwise: LoadBankMemory(%u bytes) -> stub (bank not loaded)", size);
    log_line(buf);
    if (bank_id) *bank_id = ++s_bank_count;
    return AK_SUCCESS;
}
unsigned TRT_GUEST_ABI wwise_AK_SoundEngine_UnloadBank(unsigned bank_id, const void* mem, unsigned size, void* cb) {
    return AK_SUCCESS;
}
unsigned TRT_GUEST_ABI wwise_AK_SoundEngine_UnloadBankID(unsigned bank_id) { return AK_SUCCESS; }

// ---- Event Playback ----
unsigned TRT_GUEST_ABI wwise_AK_SoundEngine_PostEvent(const char* event_name, unsigned game_obj, unsigned flags, void* cb, void* cookie) {
    char buf[256];
    snprintf(buf, sizeof(buf), "wwise: PostEvent('%s') -> stub (silent)", event_name ? event_name : "(null)");
    log_line(buf);
    return 0; // playing ID 0 = invalid
}
unsigned TRT_GUEST_ABI wwise_AK_SoundEngine_PostEventID(unsigned event_id, unsigned game_obj, unsigned flags, void* cb, void* cookie) {
    return 0;
}
unsigned TRT_GUEST_ABI wwise_AK_SoundEngine_ExecuteActionOnEvent(const char* event_name, unsigned action, unsigned game_obj, int transition, unsigned playing_id) { return AK_SUCCESS; }
unsigned TRT_GUEST_ABI wwise_AK_SoundEngine_StopAll(unsigned game_obj) { return AK_SUCCESS; }
unsigned TRT_GUEST_ABI wwise_AK_SoundEngine_StopPlayingID(unsigned playing_id, int transition) { return AK_SUCCESS; }

// ---- Game Object Management ----
unsigned TRT_GUEST_ABI wwise_AK_SoundEngine_RegisterGameObj(unsigned game_obj, const char* name) { return AK_SUCCESS; }
unsigned TRT_GUEST_ABI wwise_AK_SoundEngine_UnregisterGameObj(unsigned game_obj) { return AK_SUCCESS; }
unsigned TRT_GUEST_ABI wwise_AK_SoundEngine_SetPosition(unsigned game_obj, const void* pos, unsigned num_pos) { return AK_SUCCESS; }
unsigned TRT_GUEST_ABI wwise_AK_SoundEngine_SetListenerPosition(unsigned listener, const void* pos) { return AK_SUCCESS; }
unsigned TRT_GUEST_ABI wwise_AK_SoundEngine_SetRTPCValue(const char* rtpc, float value, unsigned game_obj, unsigned time_ms, unsigned curve) { return AK_SUCCESS; }
unsigned TRT_GUEST_ABI wwise_AK_SoundEngine_SetState(const char* state_group, const char* state) { return AK_SUCCESS; }
unsigned TRT_GUEST_ABI wwise_AK_SoundEngine_SetSwitch(const char* switch_group, const char* state, unsigned game_obj) { return AK_SUCCESS; }

// ---- Query ----
unsigned TRT_GUEST_ABI wwise_AK_SoundEngine_IsInitialized(void) { return 1; }
unsigned TRT_GUEST_ABI wwise_AK_SoundEngine_GetIDFromString(const char* str) { return 0; }

// ---- AkSoundEngine namespace variants (AK:: namespace) ----
// These are the C++ mangled names; we register unmangled for GetProcAddress

}  // extern "C"

extern "C" int trt_bridge_register_wwise(void) {
    static const trt_named_fn k_fns[] = {
        {"AK_SoundEngine_Init", (void*)&wwise_AK_SoundEngine_Init},
        {"AK_SoundEngine_Term", (void*)&wwise_AK_SoundEngine_Term},
        {"AK_SoundEngine_RenderAudio", (void*)&wwise_AK_SoundEngine_RenderAudio},
        {"AK_SoundEngine_GetDefaultInitSettings", (void*)&wwise_AK_SoundEngine_GetDefaultInitSettings},
        {"AK_SoundEngine_GetDefaultPlatformInitSettings", (void*)&wwise_AK_SoundEngine_GetDefaultPlatformInitSettings},
        {"AK_MemoryMgr_Init", (void*)&wwise_AK_MemoryMgr_Init},
        {"AK_MemoryMgr_Term", (void*)&wwise_AK_MemoryMgr_Term},
        {"AK_MemoryMgr_Malloc", (void*)&wwise_AK_MemoryMgr_Malloc},
        {"AK_MemoryMgr_Free", (void*)&wwise_AK_MemoryMgr_Free},
        {"AK_StreamMgr_Create", (void*)&wwise_AK_StreamMgr_Create},
        {"AK_StreamMgr_Destroy", (void*)&wwise_AK_StreamMgr_Destroy},
        {"AK_StreamMgr_SetFileLocationResolver", (void*)&wwise_AK_StreamMgr_SetFileLocationResolver},
        {"AK_MusicEngine_Init", (void*)&wwise_AK_MusicEngine_Init},
        {"AK_MusicEngine_Term", (void*)&wwise_AK_MusicEngine_Term},
        {"AK_SoundEngine_LoadBank", (void*)&wwise_AK_SoundEngine_LoadBank},
        {"AK_SoundEngine_LoadBankMemory", (void*)&wwise_AK_SoundEngine_LoadBankMemory},
        {"AK_SoundEngine_UnloadBank", (void*)&wwise_AK_SoundEngine_UnloadBank},
        {"AK_SoundEngine_UnloadBankID", (void*)&wwise_AK_SoundEngine_UnloadBankID},
        {"AK_SoundEngine_PostEvent", (void*)&wwise_AK_SoundEngine_PostEvent},
        {"AK_SoundEngine_PostEventID", (void*)&wwise_AK_SoundEngine_PostEventID},
        {"AK_SoundEngine_ExecuteActionOnEvent", (void*)&wwise_AK_SoundEngine_ExecuteActionOnEvent},
        {"AK_SoundEngine_StopAll", (void*)&wwise_AK_SoundEngine_StopAll},
        {"AK_SoundEngine_StopPlayingID", (void*)&wwise_AK_SoundEngine_StopPlayingID},
        {"AK_SoundEngine_RegisterGameObj", (void*)&wwise_AK_SoundEngine_RegisterGameObj},
        {"AK_SoundEngine_UnregisterGameObj", (void*)&wwise_AK_SoundEngine_UnregisterGameObj},
        {"AK_SoundEngine_SetPosition", (void*)&wwise_AK_SoundEngine_SetPosition},
        {"AK_SoundEngine_SetListenerPosition", (void*)&wwise_AK_SoundEngine_SetListenerPosition},
        {"AK_SoundEngine_SetRTPCValue", (void*)&wwise_AK_SoundEngine_SetRTPCValue},
        {"AK_SoundEngine_SetState", (void*)&wwise_AK_SoundEngine_SetState},
        {"AK_SoundEngine_SetSwitch", (void*)&wwise_AK_SoundEngine_SetSwitch},
        {"AK_SoundEngine_IsInitialized", (void*)&wwise_AK_SoundEngine_IsInitialized},
        {"AK_SoundEngine_GetIDFromString", (void*)&wwise_AK_SoundEngine_GetIDFromString},
    };
    trt_module_register("AK::SoundEngine.dll", TRT_ROUTE_BUILTIN, k_fns,
                        (int)(sizeof(k_fns) / sizeof(k_fns[0])));
    trt_module_register("AkSoundEngine.dll", TRT_ROUTE_BUILTIN, k_fns,
                        (int)(sizeof(k_fns) / sizeof(k_fns[0])));
    trt_module_register("AkSoundEngineP.dll", TRT_ROUTE_BUILTIN, k_fns,
                        (int)(sizeof(k_fns) / sizeof(k_fns[0])));
    trt_module_register("AkMusicEngine.dll", TRT_ROUTE_BUILTIN, k_fns,
                        (int)(sizeof(k_fns) / sizeof(k_fns[0])));
    trt_module_register("AkStreamMgr.dll", TRT_ROUTE_BUILTIN, k_fns,
                        (int)(sizeof(k_fns) / sizeof(k_fns[0])));
    trt_module_register("AkMemoryMgr.dll", TRT_ROUTE_BUILTIN, k_fns,
                        (int)(sizeof(k_fns) / sizeof(k_fns[0])));
    return 0;
}
