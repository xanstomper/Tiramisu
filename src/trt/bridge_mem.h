// tiramisu/src/trt/bridge_mem.h
// Memory-mapped files for the guest (bridge_mem.cpp).
#ifndef TIRAMISU_BRIDGE_MEM_H
#define TIRAMISU_BRIDGE_MEM_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

int trt_bridge_register_memory(void);
void trt_va_notify_protect(void* base, size_t size, u32 win32_prot,
                           u32* prev_prot_out);
int trt_va_mark_pending_rx(void* base, size_t size, u32* prev_prot_out);
int trt_va_try_promote_rx(u64 fault_addr);

typedef void* (*trt_vmem_reserve_fn)(size_t bytes, u64 preferred_base);
typedef int   (*trt_vmem_commit_fn)(void* base, size_t bytes, unsigned prot);
typedef int   (*trt_vmem_decommit_fn)(void* base, size_t bytes);
void trt_va_enable_deferred_data(int enabled);
int trt_va_handle_deferred_data_fault(u64 address, u32 access);
int trt_va_is_deferred_readwrite(const void* buffer, size_t size);
int trt_va_prepare_data_range(const void* buffer, size_t size);
void trt_set_vmem_hooks(trt_vmem_reserve_fn reserve_fn,
                        trt_vmem_commit_fn commit_fn,
                        trt_vmem_decommit_fn decommit_fn);
int trt_mem_close_handle(void* h);
void trt_mem_stats(u32* active_mappings, u32* active_views,
                   u64* total_view_bytes);

#ifdef __cplusplus
}
#endif

#endif
