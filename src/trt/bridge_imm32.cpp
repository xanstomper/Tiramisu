// tiramisu/src/trt/bridge_imm32.cpp
// IMM32 bridge. See bridge_imm32.h.
//
// The console has no IME: ImmGetContext returns NULL (no input-context
// record exists — the honest answer), every associate/release/set call is
// a defensive no-op that names itself in the log. Games guard on the NULL
// context and skip their IME path.
#include "bridge_imm32.h"
#include "thunk.h"
#include "trt.h"

#include <stdio.h>
#include <string.h>

namespace {
namespace immlog {
static void line(const char* s) { trt_core_log(s); }
}
#define log_line(s_) immlog::line(s_)
}  // namespace

extern "C" {

void* TRT_GUEST_ABI imm_ImmGetContext(void* hwnd) {
  static unsigned s_logged = 0;
  if (++s_logged <= 3) {
    char line[128];
    snprintf(line, sizeof(line),
             "imm32: ImmGetContext -> NULL (no IME on console; %s)", hwnd ? "hwnd" : "null hwnd");
    log_line(line);
  }
  return nullptr;
}

int TRT_GUEST_ABI imm_ImmAssociateContext(void* hwnd, void* ctx) {
  log_line("imm32: ImmAssociateContext -> NULL (no IME: nothing associated)");
  (void)hwnd; (void)ctx;
  return 0;
}

int TRT_GUEST_ABI imm_ImmAssociateContextEx(void* hwnd, void* ctx, u32 flags) {
  log_line("imm32: ImmAssociateContextEx -> FALSE (no IME on console)");
  (void)hwnd; (void)ctx; (void)flags;
  return 0;
}

int TRT_GUEST_ABI imm_ImmReleaseContext(void* hwnd, void* ctx) {
  log_line("imm32: ImmReleaseContext -> TRUE (no IME context to release)");
  (void)hwnd; (void)ctx;
  return 1;
}

int TRT_GUEST_ABI imm_ImmGetCompositionStringW(void* ctx, u32 index,
                                               void* buf, u32 len) {
  char line[160];
  snprintf(line, sizeof(line),
           "imm32: ImmGetCompositionStringW -> -1 (ERROR_INVALID_PARAMETER: "
           "no IME composition exists; index=0x%x)", index);
  log_line(line);
  (void)ctx; (void)buf; (void)len;
  return -1;
}

int TRT_GUEST_ABI imm_ImmGetCompositionStringA(void* ctx, u32 index,
                                               void* buf, u32 len) {
  return imm_ImmGetCompositionStringW(ctx, index, buf, len);
}

int TRT_GUEST_ABI imm_ImmSetCompositionStringW(void* ctx, u32 index,
                                               void* comp, u32 clen, void* read,
                                               u32 rlen) {
  log_line("imm32: ImmSetCompositionStringW -> FALSE (no IME on console)");
  (void)ctx; (void)index; (void)comp; (void)clen; (void)read; (void)rlen;
  return 0;
}

int TRT_GUEST_ABI imm_ImmNotifyIME(void* ctx, u32 action, u32 index, u32 value) {
  log_line("imm32: ImmNotifyIME -> FALSE (no IME on console)");
  (void)ctx; (void)action; (void)index; (void)value;
  return 0;
}

int TRT_GUEST_ABI imm_ImmGetConversionStatus(void* ctx, u32* conv,
                                             u32* sentence) {
  log_line("imm32: ImmGetConversionStatus -> FALSE (no IME on console)");
  (void)ctx; (void)conv; (void)sentence;
  return 0;
}

int TRT_GUEST_ABI imm_ImmSetOpenStatus(void* ctx, int open) {
  log_line("imm32: ImmSetOpenStatus -> FALSE (no IME on console)");
  (void)ctx; (void)open;
  return 0;
}

int TRT_GUEST_ABI imm_ImmGetOpenStatus(void* ctx) {
  (void)ctx;
  return 0;
}

int TRT_GUEST_ABI imm_ImmSetCandidateWindow(void* ctx, const void* candidate) {
  (void)ctx; (void)candidate;
  log_line("imm32: ImmSetCandidateWindow -> FALSE (no IME on console)");
  return 0;
}

int TRT_GUEST_ABI imm_ImmSetCompositionWindow(void* ctx, const void* compForm) {
  (void)ctx; (void)compForm;
  log_line("imm32: ImmSetCompositionWindow -> FALSE (no IME on console)");
  return 0;
}

u32 TRT_GUEST_ABI imm_ImmGetCandidateListW(void* ctx, u32 deIndex,
                                           void* lpCandList, u32 dwBufLen) {
  (void)ctx; (void)deIndex; (void)lpCandList; (void)dwBufLen;
  return 0;
}

u32 TRT_GUEST_ABI imm_ImmGetCandidateListCountW(void* ctx, u32* lpdwListCount) {
  (void)ctx;
  if (lpdwListCount) *lpdwListCount = 0;
  return 0;
}

int TRT_GUEST_ABI imm_ImmSetCompositionFontW(void* ctx, const void* lplf) {
  (void)ctx; (void)lplf;
  return 0;
}

int TRT_GUEST_ABI imm_ImmGetCompositionFontW(void* ctx, void* lplf) {
  (void)ctx; (void)lplf;
  return 0;
}

u32 TRT_GUEST_ABI imm_ImmGetVirtualKey(void* hwnd) {
  (void)hwnd;
  return 0;
}

int TRT_GUEST_ABI imm_ImmIsIME(void* hkl) {
  (void)hkl;
  return 0;
}

int TRT_GUEST_ABI imm_ImmDisableIME(u32 idThread) {
  (void)idThread;
  return 1;
}

void* TRT_GUEST_ABI imm_ImmGetDefaultIMEWnd(void* hwnd) {
  (void)hwnd;
  return nullptr;
}

// ImmGetIMEFileNameA: no IME is installed on the console, so there is no
// IME file name. 0 = ERROR_INSUFFICIENT_BUFFER-style empty name; SDL treats
// "no IME file" as "IME absent" and uses raw keyboard input.
u32 TRT_GUEST_ABI imm_ImmGetIMEFileNameA(void* hwnd, char* buf, u32 max_len) {
  log_line("imm32: ImmGetIMEFileNameA -> 0 (no IME installed on console)");
  return 0;
}

int trt_bridge_register_imm32(void) {
  static const trt_named_fn k_fns[] = {
      {"ImmGetContext", (void*)&imm_ImmGetContext},
      {"ImmAssociateContext", (void*)&imm_ImmAssociateContext},
      {"ImmAssociateContextEx", (void*)&imm_ImmAssociateContextEx},
      {"ImmReleaseContext", (void*)&imm_ImmReleaseContext},
      {"ImmGetCompositionStringW", (void*)&imm_ImmGetCompositionStringW},
      {"ImmGetCompositionStringA", (void*)&imm_ImmGetCompositionStringA},
      {"ImmSetCompositionStringW", (void*)&imm_ImmSetCompositionStringW},
      {"ImmNotifyIME", (void*)&imm_ImmNotifyIME},
      {"ImmGetConversionStatus", (void*)&imm_ImmGetConversionStatus},
      {"ImmSetOpenStatus", (void*)&imm_ImmSetOpenStatus},
      {"ImmGetOpenStatus", (void*)&imm_ImmGetOpenStatus},
      {"ImmSetCandidateWindow", (void*)&imm_ImmSetCandidateWindow},
      {"ImmSetCompositionWindow", (void*)&imm_ImmSetCompositionWindow},
      {"ImmGetCandidateListW", (void*)&imm_ImmGetCandidateListW},
      {"ImmGetCandidateListCountW", (void*)&imm_ImmGetCandidateListCountW},
      {"ImmSetCompositionFontW", (void*)&imm_ImmSetCompositionFontW},
      {"ImmGetCompositionFontW", (void*)&imm_ImmGetCompositionFontW},
      {"ImmGetVirtualKey", (void*)&imm_ImmGetVirtualKey},
      {"ImmIsIME", (void*)&imm_ImmIsIME},
      {"ImmDisableIME", (void*)&imm_ImmDisableIME},
      {"ImmGetDefaultIMEWnd", (void*)&imm_ImmGetDefaultIMEWnd},
      {"ImmGetIMEFileNameA", (void*)&imm_ImmGetIMEFileNameA},
  };
  trt_module_register("imm32.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  return 0;
}

}  // extern "C"
