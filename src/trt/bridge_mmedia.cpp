// tiramisu/src/trt/bridge_mmedia.cpp
// Small-gap bridges: msacm32, msvfw32, winspool.drv (M6 coverage).
// See bridge_mmedia.h for the design notes.
#include "bridge_mmedia.h"
#include "trt.h"
#include "thunk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

constexpr int ACMERR_NOTPOSSIBLE_ = (int)0x80006001;  // ACMERR_NOTPOSSIBLE
constexpr int MMSYSERR_ERROR_ = 1;                    // generic mm error
constexpr int MMSYSERR_NOERROR_ = 0;

void log_line(const char* s) { trt_core_log(s); }

}  // namespace

extern "C" {

// ===================== msacm32 =====================
u32 TRT_GUEST_ABI acm_GetVersion(void) {
  return 0x02200000;  // ACM 2.2 (the Win7-era version every game accepts)
}
u32 TRT_GUEST_ABI acm_StreamOpen(void** has_h, void*, const void* wfx_src,
                                 const void* wfx_dst, void*, u64, u32) {
  // WAVEFORMATEX: wFormatTag at +0 (u16)
  u16 src_tag = 1, dst_tag = 1;
  if (wfx_src) src_tag = *(const u16*)wfx_src;
  if (wfx_dst) dst_tag = *(const u16*)wfx_dst;
  // PCM (1) -> PCM (1): identity, works
  if (src_tag == 1 && dst_tag == 1) {
    if (has_h) *has_h = (void*)(uintptr_t)0xC0DE0001;
    return MMSYSERR_NOERROR_;
  }
  char line[96];
  snprintf(line, sizeof(line),
           "msacm32: StreamOpen tag %u->%u NOTPOSSIBLE (no codec)", src_tag,
           dst_tag);
  log_line(line);
  return (u32)ACMERR_NOTPOSSIBLE_;  // engine falls back to PCM assets
}
u32 TRT_GUEST_ABI acm_StreamPrepareHeader(void*, void*, u32) {
  return MMSYSERR_NOERROR_;
}
u32 TRT_GUEST_ABI acm_StreamUnprepareHeader(void*, void*, u32) {
  return MMSYSERR_NOERROR_;
}
u32 TRT_GUEST_ABI acm_StreamConvert(void*, void*, u32) {
  // identity stream (PCM->PCM) needs no conversion
  return MMSYSERR_NOERROR_;
}
u32 TRT_GUEST_ABI acm_StreamReset(void*, void*) { return MMSYSERR_NOERROR_; }
u32 TRT_GUEST_ABI acm_StreamClose(void* h, u32) {
  (void)h;
  return MMSYSERR_NOERROR_;
}
u32 TRT_GUEST_ABI acm_FormatSuggest(void*, const void*, void*, u32, u32) {
  // no codec beyond PCM identity: nothing to suggest a conversion to
  log_line("msacm32: FormatSuggest -> NOTPOSSIBLE (no codec)");
  return (u32)ACMERR_NOTPOSSIBLE_;
}
u32 TRT_GUEST_ABI acm_StreamSize(void*, u32 in_bytes, u32* out_bytes, u32) {
  // PCM identity stream: output size == input size
  if (out_bytes) *out_bytes = in_bytes;
  return MMSYSERR_NOERROR_;
}
u32 TRT_GUEST_ABI acm_FormatTagEnum(void*, void* out, u32* size, u32) {
  // ACMFORMATTAGDETAILS: cbStruct(+0), dwFormatTagIndex(+4),
  // dwFormatTag(+8), cbFormatSize(+12), szFormatTag(+16...)
  if (!out || !size || *size < 16) return MMSYSERR_ERROR_;
  u32* p = (u32*)out;
  u32 idx = p[1];
  if (idx == 0) {
    p[2] = 1;          // WAVE_FORMAT_PCM
    p[3] = 18;         // cbFormatSize of WAVEFORMATEX
    return MMSYSERR_NOERROR_;
  }
  return MMSYSERR_ERROR_;  // end of enumeration
}
u32 TRT_GUEST_ABI acm_FormatEnum(void*, void* out, u32* size, u32, u32) {
  // one PCM 44.1kHz stereo 16-bit format
  if (!out || !size || *size < 16) return MMSYSERR_ERROR_;
  u32* p = (u32*)out;
  u32 idx = p[1];
  if (idx == 0) {
    p[2] = 1;      // WAVE_FORMAT_PCM
    p[3] = 18;     // cbFormatSize
    return MMSYSERR_NOERROR_;
  }
  return MMSYSERR_ERROR_;
}
u32 TRT_GUEST_ABI acm_GetDriverHandle(void**, u32) {
  return MMSYSERR_ERROR_;  // no codecs installed
}
u32 TRT_GUEST_ABI acm_DriverEnum(void*, u32, u32) {
  return MMSYSERR_NOERROR_;  // zero drivers: engines stop probing
}
u32 TRT_GUEST_ABI acm_DriverID(void*, void**, u32) {
  return MMSYSERR_ERROR_;
}

// ===================== msvfw32 =====================
u32 TRT_GUEST_ABI msvfw_ICOpen(u32, u32, u32) {
  char line[80];
  snprintf(line, sizeof(line), "msvfw32: ICOpen (no AVI codecs)");
  log_line(line);
  return 0;  // NULL handle -> engine skips AVI logos
}
u32 TRT_GUEST_ABI msvfw_ICSendMessage(void*, u32, u32, u32) {
  return MMSYSERR_ERROR_;
}
u32 TRT_GUEST_ABI msvfw_ICInfo(u32, u32, void*) {
  return 0;  // no codec found
}
u32 TRT_GUEST_ABI msvfw_DrawDibOpen(void) {
  return 0;  // NULL -> engine falls back to D3D blits
}
u32 TRT_GUEST_ABI msvfw_AVISendMessage(void*, u32, u32, u32) {
  return MMSYSERR_ERROR_;
}
u32 TRT_GUEST_ABI msvfw_MCIWndRegisterClass(void) { return 0; }

// ===================== winspool.drv =====================
u32 TRT_GUEST_ABI wsp_OpenPrinterW(const void*, void** h, void*) {
  if (h) *h = nullptr;
  return 0;  // FALSE: no printer
}
u32 TRT_GUEST_ABI wsp_OpenPrinterA(const void*, void** h, void*) {
  if (h) *h = nullptr;
  return 0;
}
u32 TRT_GUEST_ABI wsp_ClosePrinterW(void*) { return 1; }
u32 TRT_GUEST_ABI wsp_EnumPrintersW(u32, const void*, u32, u8* buf, u32 buf_len,
                                    u32* needed, u32* returned) {
  if (needed) *needed = 0;
  if (returned) *returned = 0;
  (void)buf; (void)buf_len;
  char line[64];
  snprintf(line, sizeof(line), "winspool: EnumPrintersW (0 printers)");
  log_line(line);
  return 0;  // FALSE: no printers, engines skip print features
}
u32 TRT_GUEST_ABI wsp_EnumPrintersA(u32, const void*, u32, u8* buf, u32 buf_len,
                                    u32* needed, u32* returned) {
  return wsp_EnumPrintersW(0, nullptr, 0, buf, buf_len, needed, returned);
}
u32 TRT_GUEST_ABI wsp_GetDefaultPrinterW(void* buf, u32* size) {
  if (size) *size = 0;
  return 0;  // ERROR_FILE_NOT_FOUND by GetLastError semantics; FALSE
}
u32 TRT_GUEST_ABI wsp_GetPrinterW(void*, u32, u8*, u32, u32* needed) {
  if (needed) *needed = 0;
  return 0;
}

int trt_bridge_register_mmedia(void) {
  static const trt_named_fn k_acm[] = {
      {"acmGetVersion", (void*)&acm_GetVersion},
      {"acmStreamOpen", (void*)&acm_StreamOpen},
      {"acmStreamPrepareHeader", (void*)&acm_StreamPrepareHeader},
      {"acmStreamUnprepareHeader", (void*)&acm_StreamUnprepareHeader},
      {"acmStreamConvert", (void*)&acm_StreamConvert},
      {"acmStreamReset", (void*)&acm_StreamReset},
      {"acmStreamClose", (void*)&acm_StreamClose},
      {"acmFormatSuggest", (void*)&acm_FormatSuggest},
      {"acmStreamSize", (void*)&acm_StreamSize},
      {"acmFormatTagEnumW", (void*)&acm_FormatTagEnum},
      {"acmFormatTagEnumA", (void*)&acm_FormatTagEnum},
      {"acmFormatEnumW", (void*)&acm_FormatEnum},
      {"acmFormatEnumA", (void*)&acm_FormatEnum},
      {"acmGetDriverHandle", (void*)&acm_GetDriverHandle},
      {"acmDriverEnum", (void*)&acm_DriverEnum},
      {"acmDriverID", (void*)&acm_DriverID},
  };
  trt_module_register("msacm32.dll", TRT_ROUTE_BUILTIN, k_acm,
                      (int)(sizeof(k_acm) / sizeof(k_acm[0])));

  static const trt_named_fn k_vfw[] = {
      {"ICOpen", (void*)&msvfw_ICOpen},
      {"ICSendMessage", (void*)&msvfw_ICSendMessage},
      {"ICInfo", (void*)&msvfw_ICInfo},
      {"DrawDibOpen", (void*)&msvfw_DrawDibOpen},
      {"AVISendMessage", (void*)&msvfw_AVISendMessage},
      {"MCIWndRegisterClass", (void*)&msvfw_MCIWndRegisterClass},
  };
  trt_module_register("msvfw32.dll", TRT_ROUTE_BUILTIN, k_vfw,
                      (int)(sizeof(k_vfw) / sizeof(k_vfw[0])));

  static const trt_named_fn k_wsp[] = {
      {"OpenPrinterW", (void*)&wsp_OpenPrinterW},
      {"OpenPrinterA", (void*)&wsp_OpenPrinterA},
      {"ClosePrinterW", (void*)&wsp_ClosePrinterW},
      {"ClosePrinterA", (void*)&wsp_ClosePrinterW},
      {"EnumPrintersW", (void*)&wsp_EnumPrintersW},
      {"EnumPrintersA", (void*)&wsp_EnumPrintersA},
      {"GetDefaultPrinterW", (void*)&wsp_GetDefaultPrinterW},
      {"GetPrinterW", (void*)&wsp_GetPrinterW},
  };
  trt_module_register("winspool.drv", TRT_ROUTE_BUILTIN, k_wsp,
                      (int)(sizeof(k_wsp) / sizeof(k_wsp[0])));
  return 0;
}

}  // extern "C"
