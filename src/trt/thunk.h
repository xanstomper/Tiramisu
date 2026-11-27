// tiramisu/src/trt/thunk.h
// Fail-visible thunks.
//
// The project's rule: "STUB hits are work items; never silent success."
// A stub that returns 0 without saying so hides the gap. So every
// unresolved / unimplemented symbol gets its OWN tiny callable that names
// itself at call time:
//
//   trt: STUB advapi32.dll!RegOpenKeyExW called (unimplemented)
//   trt: DELAY d3dcompiler_47.dll!D3DCompile called (unresolved)
//
// Implementation: one x86-64 stub per symbol, allocated RW then flipped RX
// through the backend (same W^X discipline the image loader uses).
#ifndef TIRAMISU_THUNK_H
#define TIRAMISU_THUNK_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

// Remember the host backend (set once at boot by the host or the test).
void trt_core_set_backend(const trt_backend* be);
const trt_backend* trt_core_backend(void);

// Log through the backend if we have one, else stderr.
void trt_core_log(const char* line);

typedef struct trt_unsupported_api_call {
  u64 observed_calls;
  u64 return_value;
  u32 thread_id;
  char kind[16];
  char name[224];
} trt_unsupported_api_call;

// The first executed fallback in this launch and the total fallback calls.
// Bind-only lookups never count here. The snapshot contains no guest pointers.
void trt_thunk_begin_launch(void);
trt_unsupported_api_call trt_thunk_first_call(void);

// .203 diagnostic: current thread id for the [tid=] log prefix.
// Defined in bridge_kernel32.cpp (Win32: ::GetCurrentThreadId()).
u32 TRT_STDCALL trt_GetCurrentThreadId(void);

// Build (or reuse) a callable that logs "kind mod!sym called" and returns
// 0. `be` may be NULL to use the remembered backend.
void* trt_thunk_make(const trt_backend* be, const char* kind,
                     const char* mod, const char* sym);

// Build (or reuse) a callable that logs "kind mod!sym called" and returns
// ret_val. `be` may be NULL to use the remembered backend.
void* trt_thunk_make_ext(const trt_backend* be, const char* kind,
                         const char* mod, const char* sym, u64 ret_val);

#ifdef __cplusplus
}
#endif

#endif // TIRAMISU_THUNK_H
