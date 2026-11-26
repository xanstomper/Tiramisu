// tiramisu/src/trt/bridge_oodle.cpp
// Oodle (oo2core_6_win64.dll) bridge — stubs.
//
// Oodle is RAD Game Tools' compression library. Tiramisu does not implement
// it; these stubs let LoadLibrary/GetProcAddress succeed and return honest
// failure so games that probe for Oodle fall back to standard decompression.
#include "bridge_oodle.h"
#include "thunk.h"
#include "trt.h"

#include <stdio.h>

namespace {
namespace oodlelog { static void line(const char* s) { trt_core_log(s); } }
#define log_line(s_) oodlelog::line(s_)
}

extern "C" {

// OodleLZ_Decompress: int (const void* src, int src_len, void* dst, int dst_len, ...)
int TRT_GUEST_ABI oodle_OodleLZ_Decompress(const void* src, int src_len,
                                            void* dst, int dst_len, int fuzz,
                                            int crc, int verbose, int* dst_bytes) {
    log_line("oodle: OodleLZ_Decompress -> -1 (Oodle not implemented)");
    if (dst_bytes) *dst_bytes = 0;
    return -1;
}

void* TRT_GUEST_ABI oodle_Oodle_GetErrorHandler(void) { return nullptr; }
void TRT_GUEST_ABI oodle_Oodle_SetErrorHandler(void* fn) {}

}  // extern "C"

extern "C" int trt_bridge_register_oodle(void) {
    static const trt_named_fn k_fns[] = {
        {"OodleLZ_Decompress", (void*)&oodle_OodleLZ_Decompress},
        {"Oodle_GetErrorHandler", (void*)&oodle_Oodle_GetErrorHandler},
        {"Oodle_SetErrorHandler", (void*)&oodle_Oodle_SetErrorHandler},
    };
    static const char* const k_oodle_mods[] = {
        "oo2core_9_win64.dll", "oo2core_8_win64.dll", "oo2core_7_win64.dll",
        "oo2core_6_win64.dll", "oo2core_5_win64.dll", "oo2core_4_win64.dll",
        "oo2core_3_win64.dll", "oo2core_2_win64.dll", "oo2core_1_win64.dll",
        "oo2core_win64.dll",
        "oo2core_9_win32.dll", "oo2core_8_win32.dll", "oo2core_7_win32.dll",
        "oo2core_6_win32.dll", "oo2core_5_win32.dll", "oo2core_4_win32.dll",
        "oo2core_3_win32.dll", "oo2core_win32.dll",
    };
    for (size_t i = 0; i < sizeof(k_oodle_mods) / sizeof(k_oodle_mods[0]); ++i) {
        trt_module_register(k_oodle_mods[i], TRT_ROUTE_BUILTIN, k_fns,
                            (int)(sizeof(k_fns) / sizeof(k_fns[0])));
    }
    return 0;
}