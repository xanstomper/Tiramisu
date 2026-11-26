// tiramisu/src/trt/x86_cpu.h
// x86 (32-bit) CPU emulation core for Tiramisu.
//
// Provides register state, flat 4GB memory model, instruction decode/execute,
// and bridge thunking so 32-bit PE guests can run on the 64-bit UWP host.
//
// This is NOT a full emulator. It handles common integer instructions,
// memory access, control flow, and API call thunking. FPU/SSE and
// advanced features are stubbed where needed.
#ifndef TRT_X86_CPU_H
#define TRT_X86_CPU_H

#include "trt.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

// ---- x86 registers ----
struct x86_regs {
    // General purpose (32-bit)
    uint32_t eax, ebx, ecx, edx;
    uint32_t esi, edi, ebp, esp;
    // Instruction pointer
    uint32_t eip;
    // Flags (EFLAGS)
    uint32_t eflags;
    // Segment registers (mostly ignored, flat model)
    uint16_t cs, ds, es, fs, gs, ss;
    uint32_t fs_base; // FS segment base (for SEH/TEB)
    // FPU (x87) - 8 registers as doubles (host computation)
    double fpu[8];
    int fpu_top;  // top of stack index (0-7)
    uint16_t fpu_cw, fpu_sw, fpu_tw;
    // Tag word: 00=valid, 01=zero, 10=special, 11=empty
    uint8_t fpu_tags[8];
    // SSE - 8 registers, 128-bit
    uint8_t xmm[8][16];
    uint32_t mxcsr;
    // MMX - 8 registers, 64-bit (aliases x87 in real HW; kept separate here)
    uint64_t mm[8];
    // TLS slots (TEB ThreadLocalStoragePointer at FS:[0x2C])
    uint32_t tls_slot[64];
    uint8_t tls_used[64];
    // Time-stamp counter base for RDTSC
    uint64_t tsc_base;
};

// EFLAGS bits
#define X86_CF  (1 << 0)   // Carry
#define X86_PF  (1 << 2)   // Parity
#define X86_AF  (1 << 4)   // Auxiliary carry
#define X86_ZF  (1 << 6)   // Zero
#define X86_SF  (1 << 7)   // Sign
#define X86_TF  (1 << 8)   // Trap
#define X86_IF  (1 << 9)   // Interrupt enable
#define X86_DF  (1 << 10)  // Direction
#define X86_OF  (1 << 11)  // Overflow

// Intel condition codes are shared by Jcc, SETcc and CMOVcc.
static inline bool x86_condition(uint32_t flags, unsigned code) {
    const bool carry = (flags & X86_CF) != 0;
    const bool parity = (flags & X86_PF) != 0;
    const bool zero = (flags & X86_ZF) != 0;
    const bool sign = (flags & X86_SF) != 0;
    const bool overflow = (flags & X86_OF) != 0;
    switch (code) {
        case 0: return overflow;
        case 1: return !overflow;
        case 2: return carry;
        case 3: return !carry;
        case 4: return zero;
        case 5: return !zero;
        case 6: return carry || zero;
        case 7: return !carry && !zero;
        case 8: return sign;
        case 9: return !sign;
        case 10: return parity;
        case 11: return !parity;
        case 12: return sign != overflow;
        case 13: return sign == overflow;
        case 14: return zero || sign != overflow;
        case 15: return !zero && sign == overflow;
        default: return false;
    }
}

enum x86_execution_status {
    X86_EXEC_OK = 0,
    X86_EXEC_MEMORY = 1,
    X86_EXEC_BAD_INSTRUCTION = 2,
    X86_EXEC_UNHANDLED_TRAP = 3,
    X86_EXEC_JIT_FAILURE = 4,
    X86_EXEC_ACCESS_VIOLATION = 5,
    X86_EXEC_INSTRUCTION_LIMIT = 6,
    X86_EXEC_GUEST_ERROR = 7,
};

// ---- Memory model ----
// Flat 4GB guest address space backed by ONE contiguous host reservation, so a
// guest VA is directly usable as a host pointer by the 64-bit bridges. Pages
// are committed lazily and tracked by a bitmap; uncommitted accesses read 0
// and writes are dropped (never a host crash).
#define X86_PAGE_SIZE 4096
#define X86_PAGE_COUNT (1 << 20)  // 4GB / 4KB
#define X86_PAGE_SHIFT 12
#define X86_MAP_WORDS (X86_PAGE_COUNT / 64)

struct x86_mem {
    uint8_t* base;            // 4GB reservation (nullptr before first commit)
    uint64_t map[X86_MAP_WORDS]; // committed-page bitmap (1 = readable/writable)
    size_t committed;         // total committed bytes
};

// ---- Execution context ----
struct x86_context {
    x86_regs regs;
    x86_mem mem;
    // Guest image base (host pointer to mapped PE)
    void* image_base;
    uint32_t image_size;
    // The same image's GUEST base. A 32-bit guest must get this back from
    // GetModuleHandle(NULL) / a NULL hInstance, never the host pointer.
    uint32_t image_va;
    // Entry point (RVA)
    uint32_t entry_rva;
    // Is the CPU running?
    int running;
    int last_error;                  // guest Win32 GetLastError value
    x86_execution_status execution;  // interpreter run result
    // Instruction count (for debugging/timeouts)
    uint64_t insn_count;
    // Max instructions before forced stop
    uint64_t insn_limit;
    // Log callback
    void (*log)(const char*);
    // TLS array base address (guest)
    uint32_t tls_array;
    // ---- JIT state ----
    // Pages that currently hold recompiled basic blocks. A store into one of
    // these bumps `code_epoch`, which invalidates every cached block
    // (self-modifying-code safety).
    uint64_t code_pages[X86_MAP_WORDS];
    uint32_t code_epoch;
    int jit_enabled;
    void* jit_cache;          // jit_cache* (owned by x86_jit.cpp)
    // Sentinel address for guest->guest calls made by the SEH dispatcher.
    uint32_t seh_sentinel;
    uint32_t command_line_a;  // guest VA, initialized on first query
    uint32_t command_line_w;
};

void x86_halt(x86_context* ctx, uint32_t address);

// Called by guest stores that land in a page holding recompiled code.
void x86_jit_note_write(x86_context* ctx, uint32_t addr);
// Drop every cached basic block.
void x86_jit_flush(x86_context* ctx);
// Execute one cached (or freshly recompiled) basic block on this guest VA.
// Returns 1 when a block ran, 0 when the address is not recompilable (the
// caller should fall back to x86_step), -1 on a fatal guest error.
int x86_jit_step(x86_context* ctx);
// Recompiler statistics (for tests/telemetry).
struct x86_jit_stats { unsigned long long blocks, insns, decoded, evictions; };
void x86_jit_get_stats(x86_context* ctx, x86_jit_stats* out);

// ---- Structured Exception Handling ----
// Deliver an exception to the guest's FS:[0] handler chain. Returns 0 when a
// handler resumed execution (EIP/ESP/registers restored from the CONTEXT),
// -1 when the chain ended unhandled (caller decides how to fail).
int x86_raise_seh(x86_context* ctx, uint32_t code, uint32_t flags,
                  uint32_t addr, uint32_t nparams, const uint32_t* params);
// Call a guest function with __cdecl args and return its EAX.
uint32_t x86_call_guest(x86_context* ctx, uint32_t fn, const uint32_t* args, int nargs);

// ---- Memory access helpers ----
static inline int x86_page_mapped(x86_context* ctx, uint32_t page) {
    return (ctx->mem.map[page >> 6] >> (page & 63)) & 1;
}
// Is every page touched by [addr, addr+n) committed?
static inline int x86_span_mapped(x86_context* ctx, uint32_t addr, uint32_t n) {
    uint32_t p0 = addr >> X86_PAGE_SHIFT;
    uint32_t p1 = (addr + n - 1) >> X86_PAGE_SHIFT;
    if (p0 == p1) return x86_page_mapped(ctx, p0);
    if (p1 >= X86_PAGE_COUNT) return 0;
    for (uint32_t p = p0; p <= p1; p++)
        if (!x86_page_mapped(ctx, p)) return 0;
    return 1;
}

// Forward declaration (defined in x86_exec.cpp): commits a range of guest
// memory inside the 4 GB reservation.
int x86_mem_commit(x86_context* ctx, uint32_t base, size_t size);

static inline uint8_t x86_mem_read8(x86_context* ctx, uint32_t addr) {
    if (!ctx->mem.base) { ctx->execution = X86_EXEC_MEMORY; return 0; }
    uint32_t page = addr >> X86_PAGE_SHIFT;
    if (!x86_page_mapped(ctx, page)) {
        // Demand-page: commit the page on first touch, like Windows does inside
        // a reserved region. This keeps guest writes to memory the guest
        // believes exists (large static arrays, .bss overflow) from faulting
        // inside the emulator while still failing for truly out-of-range VAs.
        if (x86_mem_commit(ctx, page << X86_PAGE_SHIFT, X86_PAGE_SIZE) != 0) {
            ctx->execution = X86_EXEC_MEMORY; // page fault
            return 0;
        }
    }
    return ctx->mem.base[addr];
}

// Multi-byte accesses take a fast path that tests the page bitmap once and
// then does a single host load/store. The old byte-at-a-time form cost four
// bitmap tests plus four branches for every 32-bit operand, which dominated
// interpreter time.
static inline uint16_t x86_mem_read16(x86_context* ctx, uint32_t addr) {
    if (ctx->mem.base && (addr & 0xFFFu) <= 0xFFEu &&
        x86_page_mapped(ctx, addr >> X86_PAGE_SHIFT)) {
        uint16_t v; memcpy(&v, ctx->mem.base + addr, 2); return v;
    }
    uint16_t v = x86_mem_read8(ctx, addr);
    v |= (uint16_t)x86_mem_read8(ctx, addr + 1) << 8;
    return v;
}

static inline uint32_t x86_mem_read32(x86_context* ctx, uint32_t addr) {
    if (ctx->mem.base && (addr & 0xFFFu) <= 0xFFCu &&
        x86_page_mapped(ctx, addr >> X86_PAGE_SHIFT)) {
        uint32_t v; memcpy(&v, ctx->mem.base + addr, 4); return v;
    }
    uint32_t v = x86_mem_read8(ctx, addr);
    v |= (uint32_t)x86_mem_read8(ctx, addr + 1) << 8;
    v |= (uint32_t)x86_mem_read8(ctx, addr + 2) << 16;
    v |= (uint32_t)x86_mem_read8(ctx, addr + 3) << 24;
    return v;
}

static inline void x86_note_code_write(x86_context* ctx, uint32_t addr) {
    uint32_t page = addr >> X86_PAGE_SHIFT;
    // Only pay for the epoch bump when this page actually holds recompiled
    // code; ordinary data writes never touch it.
    if (ctx->code_pages[page >> 6] & (1ull << (page & 63))) {
        ctx->code_epoch++;
        ctx->code_pages[page >> 6] &= ~(1ull << (page & 63));
    }
}

static inline void x86_mem_write8(x86_context* ctx, uint32_t addr, uint8_t val) {
    if (!ctx->mem.base) { ctx->execution = X86_EXEC_MEMORY; return; }
    uint32_t page = addr >> X86_PAGE_SHIFT;
    if (!x86_page_mapped(ctx, page)) {
        if (x86_mem_commit(ctx, page << X86_PAGE_SHIFT, X86_PAGE_SIZE) != 0) {
            ctx->execution = X86_EXEC_MEMORY;
            return;
        }
    }
    x86_note_code_write(ctx, addr);
    ctx->mem.base[addr] = val;
}

static inline void x86_mem_write16(x86_context* ctx, uint32_t addr, uint16_t val) {
    if (ctx->mem.base && (addr & 0xFFFu) <= 0xFFEu &&
        x86_page_mapped(ctx, addr >> X86_PAGE_SHIFT)) {
        x86_note_code_write(ctx, addr);
        memcpy(ctx->mem.base + addr, &val, 2);
        return;
    }
    x86_mem_write8(ctx, addr, (uint8_t)(val & 0xFF));
    x86_mem_write8(ctx, addr + 1, (uint8_t)((val >> 8) & 0xFF));
}

static inline void x86_mem_write32(x86_context* ctx, uint32_t addr, uint32_t val) {
    if (ctx->mem.base && (addr & 0xFFFu) <= 0xFFCu &&
        x86_page_mapped(ctx, addr >> X86_PAGE_SHIFT)) {
        x86_note_code_write(ctx, addr);
        memcpy(ctx->mem.base + addr, &val, 4);
        return;
    }
    x86_mem_write8(ctx, addr, (uint8_t)(val & 0xFF));
    x86_mem_write8(ctx, addr + 1, (uint8_t)((val >> 8) & 0xFF));
    x86_mem_write8(ctx, addr + 2, (uint8_t)((val >> 16) & 0xFF));
    x86_mem_write8(ctx, addr + 3, (uint8_t)((val >> 24) & 0xFF));
}

// ---- Bulk guest-memory access (page-table backed, never host pointers) ----
static inline void xg_read(x86_context* ctx, uint32_t addr, void* out, size_t n) {
    uint8_t* p = (uint8_t*)out;
    for (size_t i = 0; i < n; i++) p[i] = x86_mem_read8(ctx, addr + (uint32_t)i);
}
static inline void xg_write(x86_context* ctx, uint32_t addr, const void* in, size_t n) {
    const uint8_t* p = (const uint8_t*)in;
    for (size_t i = 0; i < n; i++) x86_mem_write8(ctx, addr + (uint32_t)i, p[i]);
}
static inline uint64_t xg_read64(x86_context* ctx, uint32_t addr) {
    uint64_t v; xg_read(ctx, addr, &v, 8); return v;
}
static inline void xg_write64(x86_context* ctx, uint32_t addr, uint64_t v) {
    xg_write(ctx, addr, &v, 8);
}

// ---- Memory management ----
// Commit a range of guest memory (maps host pages) — declared above.

// ---- Guest heap arena (x86_heap.cpp) ----
// 32-bit guests must allocate INSIDE the 4 GB reservation, otherwise the
// pointer they keep is a truncated host address. These are the allocators
// HeapAlloc / VirtualAlloc / msvcrt malloc are routed to for such guests.
struct x86_heap_stats {
    uint32_t arena_base, arena_end, bump;
    uint64_t allocs, frees, live_bytes, peak_bytes;
};
uint32_t x86_guest_alloc(x86_context* ctx, uint32_t bytes, int zero);
uint32_t x86_guest_realloc(x86_context* ctx, uint32_t ptr, uint32_t bytes, int zero);
int x86_guest_free(x86_context* ctx, uint32_t ptr);
uint32_t x86_guest_blocksize(x86_context* ctx, uint32_t ptr);
int x86_guest_ptr_in_arena(uint32_t ptr);
void x86_guest_heap_reset(void);
void x86_guest_heap_stats(x86_heap_stats* out);
// Free all committed memory
void x86_mem_free(x86_context* ctx);
// Map a host buffer into guest address space (zero-copy for PE image)
int x86_mem_map_host(x86_context* ctx, uint32_t guest_addr, void* host_ptr, size_t size);

// ---- Run a 32-bit PE through the interpreter ----
// Takes a loaded PE32 image (host pointer + size + entry RVA)
// and runs it through the x86 interpreter.
// Returns 0 on success, -1 on error.
int x86_run_pe32(x86_context* ctx, void* image_base, size_t image_size,
                 uint32_t guest_base, uint32_t entry_rva, uint32_t stack_base,
                 size_t stack_size, const trt_image* image = nullptr);

// ---- CPU lifecycle ----
// Initialize CPU state
void x86_cpu_init(x86_context* ctx);
// Reset CPU to initial state
void x86_cpu_reset(x86_context* ctx);
// Execute one instruction, return 0 on success, -1 on error/halt
int x86_step(x86_context* ctx);
// Run until halt or insn_limit
int x86_run(x86_context* ctx);

// ---- Helpers ----
// Get register by index (0=EAX,1=ECX,2=EDX,3=EBX,4=ESP,5=EBP,6=ESI,7=EDI)
uint32_t* x86_reg_ptr(x86_context* ctx, int idx);
// Update flags for arithmetic result
void x86_update_flags_add(x86_context* ctx, uint32_t a, uint32_t b, uint32_t result);
void x86_update_flags_sub(x86_context* ctx, uint32_t a, uint32_t b, uint32_t result);
void x86_update_flags_logic(x86_context* ctx, uint32_t result);

// ---- Bridge thunking ----
// When guest calls an imported function, this resolves and calls the 64-bit bridge
// Returns the 32-bit result
uint32_t x86_call_bridge(x86_context* ctx, const char* module, const char* symbol, uint32_t* args, int nargs);

#endif // TRT_X86_CPU_H
