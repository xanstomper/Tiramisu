// tiramisu/src/trt/dynamic_api.h
// Universal Dynamic API Synthesizer & Intelligent Blanket Thunking.
//
// Automatically synthesizes fail-visible, adaptive thunks for any unknown
// or uncataloged symbol/module so no game fails at load time with TRT_ERR_IMPORT.
// Deduces return types (valid pseudo-handles, TRUE, S_OK, version numbers)
// and calling conventions (32-bit stdcall argument counts) from API signatures.
#ifndef TIRAMISU_DYNAMIC_API_H
#define TIRAMISU_DYNAMIC_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Synthesize a callable thunk for unmapped module!symbol (or ordinal).
// Returns an adaptive machine-code thunk. Never returns nullptr if backend is initialized.
void* trt_dynamic_synthesize_api(const char* module, const char* symbol, uint16_t ordinal);

// Universal COM stub object: an IUnknown-shaped object whose 256-slot vtable
// answers S_OK, so an uncataloged interface pointer can be probed instead of
// dereferencing a fake handle.
void* trt_dynamic_universal_com_object(void);

// Deduces expected return value (pseudo-handle, TRUE, S_OK, version) from API naming.
uint64_t trt_dynamic_deduce_return_val(const char* module, const char* symbol);

// Deduces stdcall argument count (number of 32-bit stack dwords to clean).
int trt_dynamic_deduce_arg_count(const char* module, const char* symbol);

// Check if a symbol was dynamically synthesized.
int trt_dynamic_is_synthesized(const char* module, const char* symbol);

#ifdef __cplusplus
}
#endif

#endif // TIRAMISU_DYNAMIC_API_H
