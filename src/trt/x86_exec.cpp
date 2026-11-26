// tiramisu/src/trt/x86_exec.cpp
// x86 (32-bit) execution engine.
//
// Decodes and executes x86 instructions. Handles common integer, memory,
// control flow, and stack operations. API calls are thunked to 64-bit
// bridges via x86_call_bridge().
#include "x86_cpu.h"

#include <cstdio>
#include <cstdlib>
#include <cmath>

// Guest object width, defined in bridge_thread.cpp: 32 makes the sync bridges
// initialize the 24-byte i386 CRITICAL_SECTION instead of Win64's 40 bytes.
extern "C" void trt_set_guest_ptr_bits(int bits);
extern "C" int trt_cs_size_for_guest(void);

static int g_seg_override = -1;

// ---- ModRM byte helpers ----
static int modrm_mod(uint8_t m) { return (m >> 6) & 3; }
static int modrm_reg(uint8_t m) { return (m >> 3) & 7; }
static int modrm_rm(uint8_t m)  { return m & 7; }

// SIB byte helpers
static int sib_scale(uint8_t s) { return (s >> 6) & 3; }
static int sib_index(uint8_t s) { return (s >> 3) & 7; }
static int sib_base(uint8_t s)  { return s & 7; }

// ---- Register index to pointer (standard x86 encoding) ----
// 0=EAX,1=ECX,2=EDX,3=EBX,4=ESP,5=EBP,6=ESI,7=EDI
uint32_t* x86_reg_ptr(x86_context* ctx, int idx) {
    switch (idx) {
        case 0: return &ctx->regs.eax;
        case 1: return &ctx->regs.ecx;
        case 2: return &ctx->regs.edx;
        case 3: return &ctx->regs.ebx;
        case 4: return &ctx->regs.esp;
        case 5: return &ctx->regs.ebp;
        case 6: return &ctx->regs.esi;
        case 7: return &ctx->regs.edi;
    }
    return &ctx->regs.eax;
}

// ---- Effective address calculation ----
// Returns the effective address for a ModRM byte. Handles mod, SIB, displacements.
static uint32_t calc_ea(x86_context* ctx, uint8_t modrm, int* bytes_consumed) {
    // CONTRACT: on entry EIP already points just past the ModRM byte, so all
    // SIB/displacement bytes are read from EIP itself. *bytes_consumed is
    // chosen so the caller's `eip += consumed - 1` advances exactly over the
    // bytes that follow ModRM.
    int mod = modrm_mod(modrm);
    int rm = modrm_rm(modrm);
    uint32_t addr = 0;

    if (mod == 3) {
        *bytes_consumed = 1;
        return 0;
    }

    int extra = 0; // bytes after ModRM (SIB + displacement)
    if (rm == 4) {
        // SIB byte follows
        uint8_t sib = x86_mem_read8(ctx, ctx->regs.eip);
        extra = 1;
        int scale = 1 << sib_scale(sib);
        int index = sib_index(sib);
        int base = sib_base(sib);
        if (index != 4) addr += *x86_reg_ptr(ctx, index) * scale;
        if (base != 5 || mod != 0) addr += *x86_reg_ptr(ctx, base);
        if (mod == 0 && base == 5) { addr += x86_mem_read32(ctx, ctx->regs.eip + 1); extra += 4; }
    } else if (mod == 0 && rm == 5) {
        // disp32 only
        addr = x86_mem_read32(ctx, ctx->regs.eip);
        extra = 4;
    } else {
        addr += *x86_reg_ptr(ctx, rm);
    }

    if (mod == 1) {
        addr += (uint32_t)(int32_t)(int8_t)x86_mem_read8(ctx, ctx->regs.eip + extra);
        extra += 1;
    } else if (mod == 2) {
        addr += x86_mem_read32(ctx, ctx->regs.eip + extra);
        extra += 4;
    }

    // FS segment override: add FS base
    if (g_seg_override == 4) {
        addr += ctx->regs.fs_base;
    }
    *bytes_consumed = extra + 1;
    return addr;
}

// ---- Flag helpers ----
void x86_update_flags_add(x86_context* ctx, uint32_t a, uint32_t b, uint32_t result) {
    ctx->regs.eflags &= ~(X86_CF | X86_ZF | X86_SF | X86_OF | X86_PF | X86_AF);
    if (result == 0) ctx->regs.eflags |= X86_ZF;
    if (result & 0x80000000) ctx->regs.eflags |= X86_SF;
    if (result < a) ctx->regs.eflags |= X86_CF; // carry
    // overflow
    if (((a ^ result) & (b ^ result)) & 0x80000000) ctx->regs.eflags |= X86_OF;
    // parity (even number of set bits in low byte)
    int bits = __builtin_popcount(result & 0xFF);
    if (bits % 2 == 0) ctx->regs.eflags |= X86_PF;
}

void x86_update_flags_sub(x86_context* ctx, uint32_t a, uint32_t b, uint32_t result) {
    ctx->regs.eflags &= ~(X86_CF | X86_ZF | X86_SF | X86_OF | X86_PF | X86_AF);
    if (result == 0) ctx->regs.eflags |= X86_ZF;
    if (result & 0x80000000) ctx->regs.eflags |= X86_SF;
    if (a < b) ctx->regs.eflags |= X86_CF; // borrow
    if (((a ^ b) & (a ^ result)) & 0x80000000) ctx->regs.eflags |= X86_OF;
    int bits = __builtin_popcount(result & 0xFF);
    if (bits % 2 == 0) ctx->regs.eflags |= X86_PF;
}

// ADC and SBB need one extra bit for carry/borrow before truncating.
static uint32_t alu_with_carry(x86_context* ctx, uint32_t lhs, uint32_t rhs,
                               unsigned width, bool subtract) {
    const auto limit = uint64_t{1} << width;
    const auto mask = uint32_t(limit - 1);
    const auto sign = uint32_t(limit >> 1);
    lhs &= mask;
    rhs &= mask;
    const auto rhs_with_carry = uint64_t(rhs) + ((ctx->regs.eflags & X86_CF) != 0);
    const auto full = subtract ? uint64_t(lhs) - rhs_with_carry : uint64_t(lhs) + rhs_with_carry;
    const auto result = uint32_t(full) & mask;
    const bool carry = subtract ? lhs < rhs_with_carry : full >= limit;
    const auto overflow = (subtract ? lhs ^ rhs : ~(lhs ^ rhs)) & (lhs ^ result) & sign;
    ctx->regs.eflags &= ~(X86_CF | X86_OF | X86_AF | X86_SF | X86_ZF | X86_PF);
    if (carry) ctx->regs.eflags |= X86_CF;
    if (overflow) ctx->regs.eflags |= X86_OF;
    if ((lhs ^ rhs ^ result) & 0x10) ctx->regs.eflags |= X86_AF;
    if (result & sign) ctx->regs.eflags |= X86_SF;
    if (result == 0) ctx->regs.eflags |= X86_ZF;
    if ((__builtin_popcount(result & 0xFF) & 1) == 0) ctx->regs.eflags |= X86_PF;
    return result;
}

void x86_update_flags_logic(x86_context* ctx, uint32_t result) {
    ctx->regs.eflags &= ~(X86_CF | X86_ZF | X86_SF | X86_OF | X86_PF);
    if (result == 0) ctx->regs.eflags |= X86_ZF;
    if (result & 0x80000000) ctx->regs.eflags |= X86_SF;
    int bits = __builtin_popcount(result & 0xFF);
    if (bits % 2 == 0) ctx->regs.eflags |= X86_PF;
}

// ---- FPU helpers ----
static double fpu_st(x86_context* ctx, int i) {
    int idx = (ctx->regs.fpu_top + i) & 7;
    return ctx->regs.fpu[idx];
}
static void fpu_push(x86_context* ctx, double val) {
    ctx->regs.fpu_top = (ctx->regs.fpu_top - 1 + 8) & 7;
    ctx->regs.fpu[ctx->regs.fpu_top] = val;
    ctx->regs.fpu_tags[ctx->regs.fpu_top] = 0; // valid
}
static void fpu_pop(x86_context* ctx) {
    ctx->regs.fpu_tags[ctx->regs.fpu_top] = 3; // empty
    ctx->regs.fpu_top = (ctx->regs.fpu_top + 1) & 7;
}
static double fpu_round_prec(x86_context* ctx, double v);
static void fpu_set_st(x86_context* ctx, int i, double val) {
    int idx = (ctx->regs.fpu_top + i) & 7;
    ctx->regs.fpu[idx] = fpu_round_prec(ctx, val);
    ctx->regs.fpu_tags[idx] = 0;
}
// Convert with the x87 control-word rounding mode (FIST/FISTP semantics).
static long long fpu_to_int(x86_context* ctx, double v) {
    switch ((ctx->regs.fpu_cw >> 10) & 3) {
        case 0: return (long long)nearbyint(v); // nearest-even
        case 1: return (long long)floor(v);     // toward -inf
        case 2: return (long long)ceil(v);      // toward +inf
        default: return (long long)trunc(v);    // toward zero
    }
}
// Round a result to the control-word precision (24/53/64-bit significand).
static double fpu_round_prec(x86_context* ctx, double v) {
    int prec = (ctx->regs.fpu_cw >> 8) & 3;
    if (prec == 0) return (double)(float)v; // single precision
    return v; // double/extended both fit a host double
}
// Pack the x87 environment (FNSTENV layout, 28 bytes).
static void fpu_write_env(x86_context* ctx, uint32_t addr) {
    x86_mem_write16(ctx, addr + 0, ctx->regs.fpu_cw);
    x86_mem_write16(ctx, addr + 4, ctx->regs.fpu_sw);
    x86_mem_write16(ctx, addr + 8, ctx->regs.fpu_tw);
    x86_mem_write32(ctx, addr + 12, 0);
    x86_mem_write32(ctx, addr + 16, 0);
    x86_mem_write32(ctx, addr + 20, 0);
    x86_mem_write32(ctx, addr + 24, 0);
}
static void fpu_read_env(x86_context* ctx, uint32_t addr) {
    ctx->regs.fpu_cw = x86_mem_read16(ctx, addr + 0);
    ctx->regs.fpu_sw = x86_mem_read16(ctx, addr + 4);
    ctx->regs.fpu_tw = x86_mem_read16(ctx, addr + 8);
}
// FNSAVE (108 bytes): env + 8 x 80-bit register slots (stored as 64-bit here).
static void fpu_write_save(x86_context* ctx, uint32_t addr) {
    fpu_write_env(ctx, addr);
    for (int i = 0; i < 8; i++) {
        int idx = (ctx->regs.fpu_top + i) & 7;
        double v = ctx->regs.fpu[idx];
        uint64_t raw; memcpy(&raw, &v, 8);
        xg_write64(ctx, addr + 28 + i * 10, raw);
        x86_mem_write16(ctx, addr + 28 + i * 10 + 8, 0xFFFF); // exponent sentinel
    }
    memset(ctx->regs.fpu, 0, sizeof(ctx->regs.fpu));
    for (int i = 0; i < 8; i++) ctx->regs.fpu_tags[i] = 3;
    ctx->regs.fpu_top = 0;
    ctx->regs.fpu_cw = 0x037F;
}
static void fpu_read_save(x86_context* ctx, uint32_t addr) {
    fpu_read_env(ctx, addr);
    for (int i = 0; i < 8; i++) {
        uint64_t raw = xg_read64(ctx, addr + 28 + i * 10);
        double v; memcpy(&v, &raw, 8);
        int idx = (ctx->regs.fpu_top + i) & 7;
        ctx->regs.fpu[idx] = v;
        ctx->regs.fpu_tags[idx] = 0;
    }
}

// ---- Helper: read immediate based on size ----
static uint32_t read_imm8(x86_context* ctx) { uint8_t v = x86_mem_read8(ctx, ctx->regs.eip++); return v; }
static uint32_t read_imm16(x86_context* ctx) { uint16_t v = x86_mem_read16(ctx, ctx->regs.eip); ctx->regs.eip += 2; return v; }
static uint32_t read_imm32(x86_context* ctx) { uint32_t v = x86_mem_read32(ctx, ctx->regs.eip); ctx->regs.eip += 4; return v; }


// ---- SSE/SSE2/MMX operand helpers (guest memory is page-table backed) ----
static inline void sse_fetch(x86_context* ctx, uint8_t modrm, uint8_t* out16) {
    int rm = modrm_rm(modrm);
    if (modrm_mod(modrm) == 3) {
        memcpy(out16, ctx->regs.xmm[rm], 16);
    } else {
        int c = 0;
        uint32_t a = calc_ea(ctx, modrm, &c);
        ctx->regs.eip += c - 1;
        xg_read(ctx, a, out16, 16);
    }
}
static inline void sse_store(x86_context* ctx, uint8_t modrm, const uint8_t* in16) {
    int rm = modrm_rm(modrm);
    if (modrm_mod(modrm) == 3) {
        memcpy(ctx->regs.xmm[rm], in16, 16);
    } else {
        int c = 0;
        uint32_t a = calc_ea(ctx, modrm, &c);
        ctx->regs.eip += c - 1;
        xg_write(ctx, a, in16, 16);
    }
}
// Fetch 64-bit r/m operand: MMX register file when use_mmx, else low 64 of XMM
static inline void sse_fetch64(x86_context* ctx, uint8_t modrm, int use_mmx, uint8_t* out16) {
    memset(out16, 0, 16);
    int rm = modrm_rm(modrm);
    if (modrm_mod(modrm) == 3) {
        if (use_mmx) memcpy(out16, &ctx->regs.mm[rm], 8);
        else memcpy(out16, ctx->regs.xmm[rm], 8);
    } else {
        int c = 0;
        uint32_t a = calc_ea(ctx, modrm, &c);
        ctx->regs.eip += c - 1;
        xg_read(ctx, a, out16, 8);
    }
}
static inline void sse_store64(x86_context* ctx, uint8_t modrm, int use_mmx, const uint8_t* in16) {
    int rm = modrm_rm(modrm);
    if (modrm_mod(modrm) == 3) {
        if (use_mmx) memcpy(&ctx->regs.mm[rm], in16, 8);
        else memcpy(ctx->regs.xmm[rm], in16, 8);
    } else {
        int c = 0;
        uint32_t a = calc_ea(ctx, modrm, &c);
        ctx->regs.eip += c - 1;
        xg_write(ctx, a, in16, 8);
    }
}


// ---- Vector element helpers for MMX/SSE integer ops ----
enum { VADD, VSUB, VAND, VOR, VXOR, VEQ, VGT, VMAXU, VMINU, VAVG };
static inline void vec_op8(int op, uint8_t* d, const uint8_t* s, int n) {
    for (int i = 0; i < n; i++) {
        uint8_t a = d[i], b = s[i];
        switch (op) {
            case VADD: d[i] = (uint8_t)(a + b); break;
            case VSUB: d[i] = (uint8_t)(a - b); break;
            case VAND: d[i] = a & b; break;
            case VOR:  d[i] = a | b; break;
            case VXOR: d[i] = a ^ b; break;
            case VEQ:  d[i] = (a == b) ? 0xFF : 0x00; break;
            case VGT:  d[i] = ((int8_t)a > (int8_t)b) ? 0xFF : 0x00; break;
            case VMAXU: d[i] = a > b ? a : b; break;
            case VMINU: d[i] = a < b ? a : b; break;
            case VAVG: d[i] = (uint8_t)((a + b + 1) >> 1); break;
        }
    }
}
static inline void vec_op16(int op, uint8_t* d, const uint8_t* s, int n) {
    for (int i = 0; i < n; i++) {
        uint16_t a, b; memcpy(&a, d + i * 2, 2); memcpy(&b, s + i * 2, 2);
        uint16_t r;
        switch (op) {
            case VADD: r = (uint16_t)(a + b); break;
            case VSUB: r = (uint16_t)(a - b); break;
            case VAND: r = a & b; break;
            case VOR:  r = a | b; break;
            case VXOR: r = a ^ b; break;
            case VEQ:  r = (a == b) ? 0xFFFF : 0; break;
            case VGT:  r = ((int16_t)a > (int16_t)b) ? 0xFFFF : 0; break;
            case VMAXU: r = a > b ? a : b; break;
            case VMINU: r = a < b ? a : b; break;
            case VAVG: r = (uint16_t)((a + b + 1) >> 1); break;
            default: r = 0; break;
        }
        memcpy(d + i * 2, &r, 2);
    }
}
static inline void vec_op32(int op, uint8_t* d, const uint8_t* s, int n) {
    for (int i = 0; i < n; i++) {
        uint32_t a, b; memcpy(&a, d + i * 4, 4); memcpy(&b, s + i * 4, 4);
        uint32_t r;
        switch (op) {
            case VADD: r = a + b; break;
            case VSUB: r = a - b; break;
            case VAND: r = a & b; break;
            case VOR:  r = a | b; break;
            case VXOR: r = a ^ b; break;
            case VEQ:  r = (a == b) ? 0xFFFFFFFFu : 0; break;
            case VGT:  r = ((int32_t)a > (int32_t)b) ? 0xFFFFFFFFu : 0; break;
            case VMAXU: r = a > b ? a : b; break;
            case VMINU: r = a < b ? a : b; break;
            default: r = 0; break;
        }
        memcpy(d + i * 4, &r, 4);
    }
}

static inline void sse_fetch_ivec(x86_context* ctx, uint8_t modrm, int use_mmx, uint8_t* out16) {
    if (use_mmx) sse_fetch64(ctx, modrm, 1, out16);
    else sse_fetch(ctx, modrm, out16);
}
static inline void sse_store_n(x86_context* ctx, uint8_t modrm, const uint8_t* in, int n) {
    int rm = modrm_rm(modrm);
    if (modrm_mod(modrm) == 3) {
        memcpy(ctx->regs.xmm[rm], in, n);
    } else {
        int c = 0;
        uint32_t a = calc_ea(ctx, modrm, &c);
        ctx->regs.eip += c - 1;
        xg_write(ctx, a, in, n);
    }
}

// ---- Floating-point vector arithmetic helpers ----
enum { VA_ADD, VA_MUL, VA_SUB, VA_DIV, VA_SQRT, VA_MIN, VA_MAX };
static inline void sse_arith_f32(int op, uint8_t* d, const uint8_t* s, int n) {
    float a[4]; memcpy(a, d, 16);
    float b[4]; memcpy(b, s, 16);
    for (int i = 0; i < n && i < 4; i++) {
        switch (op) {
            case VA_ADD: a[i] += b[i]; break;
            case VA_MUL: a[i] *= b[i]; break;
            case VA_SUB: a[i] -= b[i]; break;
            case VA_DIV: a[i] /= b[i]; break;
            case VA_SQRT: a[i] = sqrtf(a[i]); break;
            case VA_MIN: a[i] = a[i] < b[i] ? a[i] : b[i]; break;
            case VA_MAX: a[i] = a[i] > b[i] ? a[i] : b[i]; break;
        }
    }
    memset(d, 0, 16);
    memcpy(d, a, sizeof(float) * (n < 4 ? n : 4));
    (void)b;
}
static inline void sse_arith_f64(int op, uint8_t* d, const uint8_t* s, int n) {
    double a[2]; memcpy(a, d, 16);
    double b[2]; memcpy(b, s, 16);
    for (int i = 0; i < n && i < 2; i++) {
        switch (op) {
            case VA_ADD: a[i] += b[i]; break;
            case VA_MUL: a[i] *= b[i]; break;
            case VA_SUB: a[i] -= b[i]; break;
            case VA_DIV: a[i] /= b[i]; break;
            case VA_SQRT: a[i] = sqrt(a[i]); break;
            case VA_MIN: a[i] = a[i] < b[i] ? a[i] : b[i]; break;
            case VA_MAX: a[i] = a[i] > b[i] ? a[i] : b[i]; break;
        }
    }
    memset(d, 0, 16);
    memcpy(d, a, sizeof(double) * (n < 2 ? n : 2));
    (void)b;
}
// Integer shift helpers
static inline void sse_shift16(uint8_t* d, int count, int left, int arithmetic, int n) {
    for (int i = 0; i < n; i++) {
        uint16_t v; memcpy(&v, d + i * 2, 2);
        uint16_t r;
        if (left) r = (uint16_t)(v << (count & 15));
        else if (arithmetic) r = (uint16_t)((int16_t)v >> (count & 15));
        else r = (uint16_t)(v >> (count & 15));
        memcpy(d + i * 2, &r, 2);
    }
}
static inline void sse_shift32(uint8_t* d, int count, int left, int arithmetic, int n) {
    for (int i = 0; i < n; i++) {
        uint32_t v; memcpy(&v, d + i * 4, 4);
        uint32_t r;
        if (left) r = (uint32_t)(v << (count & 31));
        else if (arithmetic) r = (uint32_t)((int32_t)v >> (count & 31));
        else r = (uint32_t)(v >> (count & 31));
        memcpy(d + i * 4, &r, 4);
    }
}
static inline void sse_shift64(uint8_t* d, int count, int left) {
    uint64_t v; memcpy(&v, d, 8);
    uint64_t r = left ? (v << (count & 63)) : (v >> (count & 63));
    memcpy(d, &r, 8);
}


// ---- 8-bit register access and flag updates ----
static inline uint8_t* r8p(x86_context* ctx, int i) {
    switch (i) {
        case 0: return (uint8_t*)&ctx->regs.eax;
        case 1: return (uint8_t*)&ctx->regs.ecx;
        case 2: return (uint8_t*)&ctx->regs.edx;
        case 3: return (uint8_t*)&ctx->regs.ebx;
        case 4: return ((uint8_t*)&ctx->regs.eax) + 1;
        case 5: return ((uint8_t*)&ctx->regs.ecx) + 1;
        case 6: return ((uint8_t*)&ctx->regs.edx) + 1;
        case 7: return ((uint8_t*)&ctx->regs.ebx) + 1;
    }
    return (uint8_t*)&ctx->regs.eax;
}
static inline void flag_szp8(x86_context* ctx, uint8_t r) {
    ctx->regs.eflags &= ~(X86_ZF | X86_SF | X86_PF);
    if (r == 0) ctx->regs.eflags |= X86_ZF;
    if (r & 0x80) ctx->regs.eflags |= X86_SF;
    if (__builtin_popcount(r) % 2 == 0) ctx->regs.eflags |= X86_PF;
}
static inline void flag_add8(x86_context* ctx, uint8_t a, uint8_t b, unsigned carry, unsigned r) {
    ctx->regs.eflags &= ~(X86_CF | X86_OF | X86_AF);
    if (r > 0xFF) ctx->regs.eflags |= X86_CF;
    if (((a ^ b ^ r) & 0x10)) ctx->regs.eflags |= X86_AF;
    if (~(a ^ b) & (a ^ r) & 0x80) ctx->regs.eflags |= X86_OF;
    flag_szp8(ctx, (uint8_t)r);
    (void)carry;
}
static inline void flag_sub8(x86_context* ctx, uint8_t a, uint8_t b, unsigned borrow, unsigned r) {
    ctx->regs.eflags &= ~(X86_CF | X86_OF | X86_AF);
    if ((int)r < 0) ctx->regs.eflags |= X86_CF;
    if (((a ^ b ^ r) & 0x10)) ctx->regs.eflags |= X86_AF;
    if ((a ^ b) & (a ^ r) & 0x80) ctx->regs.eflags |= X86_OF;
    flag_szp8(ctx, (uint8_t)r);
    (void)borrow;
}
static inline void flag_logic8(x86_context* ctx, uint8_t r) {
    ctx->regs.eflags &= ~(X86_CF | X86_OF | X86_AF);
    flag_szp8(ctx, r);
}

// Low byte addressed by an 8-bit register operand: rm 0-3 are AL/CL/DL/BL and
// rm 4-7 are AH/CH/DH/BH (the second byte of EAX/ECX/EDX/EBX).
static uint8_t read_r8(x86_context* ctx, int idx) {
    uint32_t v = *x86_reg_ptr(ctx, idx);
    return (idx < 4) ? (uint8_t)v : (uint8_t)(v >> 8);
}

// ---- Main execution step ----
// ---- 32-bit import trap dispatch (PE32 Phase 2) ----
// Imported 32-bit calls land on a reserved trap address. Dispatch to the
// 64-bit bridge, then emulate the callee's RET: pop the return address and,
// for __stdcall/__fastcall, the arguments (callee-cleans).
extern "C" int trt_pe32_is_trap(uint32_t guest_va);
extern "C" int trt_pe32_trap_info(int index, char* mod_out, size_t mod_cap,
                                 char* sym_out, size_t sym_cap,
                                 uint16_t* ordinal_out, uint32_t* trap_va_out);
extern "C" uint32_t x86_dispatch_import(x86_context* ctx, int trap_index,
                                       const char* module, const char* symbol,
                                       int* out_nargs, int* out_cdecl);

// ---- Hot-loop probe --------------------------------------------------------
// A guest that spins forever inside a small address window is indistinguishable
// from a slow boot: the run ends on the instruction limit with no evidence of
// WHERE it was stuck. (OpenTTD spent its whole budget inside 0xE56053..0xE56093,
// the MSVC CRT init-term loop, and the only symptom was "200M instructions".)
//
// This probe watches the EIP window: once TRT_X86_HOTLOOP instructions
// (default 20,000,000) have executed without leaving a 256-byte window, it
// logs ONE diagnostic line naming the window, every register, and the last
// import that ran. It never changes execution - the loop still ends on its own
// instruction limit. TRT_X86_HOTLOOP=0 disables it.
namespace {

struct hotloop_state {
    x86_context* ctx = nullptr;
    uint32_t lo = 0, hi = 0;   // current EIP window
    uint64_t in_window = 0;    // instructions executed inside it
    int reported = 0;          // one report per visited window
};

thread_local hotloop_state g_hot;
// Set by x86_trap_dispatch: the bridge that ran last, so a stalled loop can
// say whether it ever called back into the host.
thread_local char g_x86_last_import[112] = {0};

// Test/harness seam. Kept in this translation unit (not x86_cpu.h) so a
// probe-only change never forces every object that includes the CPU header to
// rebuild.
static bool g_hotloop_override_set = false;
static uint64_t g_hotloop_override = 0;

static uint64_t hotloop_limit_from_env(void) {
    if (g_hotloop_override_set) return g_hotloop_override;
    const char* e = getenv("TRT_X86_HOTLOOP");
    if (!e || !e[0]) return 20000000ull;
    uint64_t v = 0;
    for (const char* p = e; *p >= '0' && *p <= '9'; ++p)
        v = v * 10u + (uint64_t)(*p - '0');
    return v;  // 0 disables the probe
}

// Called once per executed instruction by x86_run().
static void hotloop_tick(x86_context* ctx, uint64_t limit) {
    if (!limit) return;
    const uint32_t eip = ctx->regs.eip;
    hotloop_state& h = g_hot;
    if (h.ctx != ctx) {
        h = hotloop_state();
        h.ctx = ctx;
        h.lo = h.hi = eip;
    }
    // Leaving the window by more than 256 bytes starts a new epoch, so a
    // normally advancing program never accumulates a stale count.
    if (eip < h.lo) {
        if (h.hi - eip > 256) { h.lo = h.hi = eip; h.in_window = 0; h.reported = 0; }
        else h.lo = eip;
    } else if (eip > h.hi) {
        if (eip - h.lo > 256) { h.lo = h.hi = eip; h.in_window = 0; h.reported = 0; }
        else h.hi = eip;
    }
    ++h.in_window;
    if (h.reported || h.in_window < limit) return;
    h.reported = 1;
    if (!ctx->log) return;
    char line[512];
    snprintf(line, sizeof(line),
             "x86: HOT LOOP 0x%08X..0x%08X - %llu instructions inside a %u-byte "
             "window without leaving it (last bridge: %s); regs eip=%08X "
             "esp=%08X ebp=%08X eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X "
             "edi=%08X eflags=%08X",
             h.lo, h.hi, (unsigned long long)h.in_window,
             (unsigned)(h.hi - h.lo + 1u),
             g_x86_last_import[0] ? g_x86_last_import : "(none)",
             eip, ctx->regs.esp, ctx->regs.ebp, ctx->regs.eax, ctx->regs.ebx,
             ctx->regs.ecx, ctx->regs.edx, ctx->regs.esi, ctx->regs.edi,
             ctx->regs.eflags);
    ctx->log(line);
}

// ---- EIP profile ----------------------------------------------------------
// The hot-loop probe names a TIGHT spin; a guest that wanders a wide region
// (OpenTTD's UCRT/CRT startup) instead just exhausts the instruction limit
// with no evidence of where the budget went. Sample EIP every 64 instructions
// into 64 KB buckets and report the three hottest regions when the run ends.
// Diagnostics only: no effect on execution.
// Exact 64 KB cover of the whole 4 GB space (65536 buckets, no aliasing), plus
// a heavy-hitter table that names the hottest INDIVIDUAL instructions: a
// direct-mapped slot is evicted only once its count decays to zero
// (space-saving algorithm), which pins the loop without a full hash table.
#define X86_PROFILE_BUCKETS 65536u
static uint32_t g_eip_profile[X86_PROFILE_BUCKETS];
static uint64_t g_eip_samples = 0;

struct eip_hot_slot { uint32_t eip; uint32_t hits; };
#define X86_HOT_SLOTS 16384u
static eip_hot_slot g_eip_hot[X86_HOT_SLOTS];

static void eip_profile_reset(void) {
    memset(g_eip_profile, 0, sizeof(g_eip_profile));
    memset(g_eip_hot, 0, sizeof(g_eip_hot));
    g_eip_samples = 0;
}

static void eip_profile_sample(uint32_t eip) {
    ++g_eip_profile[(eip >> 16) & (X86_PROFILE_BUCKETS - 1u)];
    ++g_eip_samples;
    eip_hot_slot& s = g_eip_hot[(eip >> 2) & (X86_HOT_SLOTS - 1u)];
    if (s.eip == eip) ++s.hits;
    else if (s.hits == 0) { s.eip = eip; s.hits = 1; }
    else --s.hits;
}

static void eip_profile_report(x86_context* ctx) {
    if (!ctx->log || ctx->insn_count < 1000000ull || !g_eip_samples) return;
    uint32_t best[3] = {0, 0, 0};
    for (uint32_t i = 1; i < X86_PROFILE_BUCKETS; ++i) {
        const uint32_t v = g_eip_profile[i];
        if (v > g_eip_profile[best[0]]) {
            best[2] = best[1]; best[1] = best[0]; best[0] = i;
        } else if (v > g_eip_profile[best[1]]) {
            best[2] = best[1]; best[1] = i;
        } else if (v > g_eip_profile[best[2]]) {
            best[2] = i;
        }
    }
    const double total = (double)g_eip_samples;
    char line[420];
    snprintf(line, sizeof(line),
             "x86: eip profile after %llu instructions (sampled every 64): "
             "0x%08X..0x%08X %.1f%%, 0x%08X..0x%08X %.1f%%, "
             "0x%08X..0x%08X %.1f%%",
             (unsigned long long)ctx->insn_count,
             best[0] << 16, (best[0] << 16) | 0xFFFFu,
             100.0 * g_eip_profile[best[0]] / total,
             best[1] << 16, (best[1] << 16) | 0xFFFFu,
             100.0 * g_eip_profile[best[1]] / total,
             best[2] << 16, (best[2] << 16) | 0xFFFFu,
             100.0 * g_eip_profile[best[2]] / total);
    ctx->log(line);

    // Hottest individual instructions: up to five slots, strongly ordered.
    uint32_t top_eip[5] = {0, 0, 0, 0, 0};
    uint32_t top_hits[5] = {0, 0, 0, 0, 0};
    for (uint32_t i = 0; i < X86_HOT_SLOTS; ++i) {
        const uint32_t h = g_eip_hot[i].hits;
        if (!h) continue;
        for (int s = 0; s < 5; ++s) {
            if (h > top_hits[s]) {
                for (int m = 4; m > s; --m) {
                    top_hits[m] = top_hits[m - 1];
                    top_eip[m] = top_eip[m - 1];
                }
                top_hits[s] = h;
                top_eip[s] = g_eip_hot[i].eip;
                break;
            }
        }
    }
    char line2[256];
    int n = snprintf(line2, sizeof(line2), "x86: hot instructions:");
    for (int s = 0; s < 5 && top_hits[s]; ++s) {
        n += snprintf(line2 + n, sizeof(line2) - (size_t)n, " 0x%08X %.1f%%",
                      top_eip[s], 100.0 * top_hits[s] / total);
        if ((size_t)n >= sizeof(line2) - 32) break;
    }
    ctx->log(line2);
}

}  // namespace

// Override the hot-loop threshold for the next x86_run() calls (0 disables).
// Used by tests and by a harness that wants the diagnostic earlier than 20M.
extern "C" void x86_set_hotloop_limit(uint64_t limit) {
    g_hotloop_override_set = true;
    g_hotloop_override = limit;
}

static int x86_trap_dispatch(x86_context* ctx) {
    if (!trt_pe32_is_trap(ctx->regs.eip)) return 0;
    int index = (int)((ctx->regs.eip - 0xF0000000u) / 4u);
    char mod[64] = {0}, sym[128] = {0};
    uint16_t ord = 0;
    uint32_t tva = 0;
    if (trt_pe32_trap_info(index, mod, sizeof(mod), sym, sizeof(sym), &ord, &tva) != 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "x86: bad trap 0x%08X", ctx->regs.eip);
        if (ctx->log) ctx->log(buf);
        ctx->running = 0;
        ctx->execution = X86_EXEC_UNHANDLED_TRAP;
        return -1;
    }
    snprintf(g_x86_last_import, sizeof(g_x86_last_import), "%s!%s", mod,
             sym[0] ? sym : "#ordinal");
    ctx->insn_count++;
    int nargs = 0, cdecl_call = 0;
    uint32_t result = x86_dispatch_import(ctx, index, mod, sym, &nargs, &cdecl_call);
    ctx->regs.eax = result;
    if (!ctx->running) return 1;
    uint32_t ret = x86_mem_read32(ctx, ctx->regs.esp);
    // TRT_X86_LOG_TRAPS=1 prints one line per import dispatch. The stack
    // pointer is the interesting part: a wrong stdcall arity shows up as a
    // steady upward drift of esp across calls, and the return address it
    // finally pops is the first address outside every image.
    if (getenv("TRT_X86_LOG_TRAPS")) {
        char tb[224];
        snprintf(tb, sizeof(tb),
                 "TRAP %s!%s nargs=%d cdecl=%d esp=%08X ret=%08X",
                 mod, sym[0] ? sym : "#ordinal", nargs, cdecl_call,
                 ctx->regs.esp, ret);
        if (ctx->log) ctx->log(tb);
    }
    ctx->regs.esp += 4;               // pop return address
    if (!cdecl_call) ctx->regs.esp += (uint32_t)(nargs * 4); // callee cleanup
    ctx->regs.eip = ret;
    return 1;
}

// ---- Structured Exception Handling ----
// Call a guest function with __cdecl arguments. Returns EAX. The sentinel page
// holds HLT, so the run loop stops when the callee returns into it.
uint32_t x86_call_guest(x86_context* ctx, uint32_t fn, const uint32_t* args, int nargs) {
    if (!ctx->seh_sentinel || !ctx->running) return 0;
    uint32_t saved_eip = ctx->regs.eip;
    uint32_t saved_esp = ctx->regs.esp;

    uint32_t sp = ctx->regs.esp;
    for (int i = nargs - 1; i >= 0; i--) {
        sp -= 4;
        x86_mem_write32(ctx, sp, args ? args[i] : 0);
    }
    sp -= 4;
    x86_mem_write32(ctx, sp, ctx->seh_sentinel);
    ctx->regs.esp = sp;
    ctx->regs.eip = fn;

    // Handlers are short; a bound keeps a broken handler from wedging us.
    long guard = 0;
    while (ctx->running && ctx->regs.eip != ctx->seh_sentinel && guard++ < 2000000L) {
        int r = x86_jit_step(ctx);
        if (r < 0) break;
        if (r == 0 && x86_step(ctx) != 0) break;
    }

    uint32_t ret = ctx->regs.eax;
    ctx->regs.eip = saved_eip;
    ctx->regs.esp = saved_esp;
    return ret;
}

// CONTEXT (x86) field offsets used by MSVC's exception handlers.
static const uint32_t XC_CONTEXT_FLAGS = 0x00, XC_SEG_GS = 0x90, XC_SEG_FS = 0x94;
static const uint32_t XC_EDI = 0xA0, XC_ESI = 0xA4, XC_EBX = 0xA8, XC_EDX = 0xAC;
static const uint32_t XC_ECX = 0xB0, XC_EAX = 0xB4, XC_EBP = 0xB8, XC_EIP = 0xBC;
static const uint32_t XC_SEG_CS = 0xC0, XC_EFLAGS = 0xC4, XC_ESP = 0xC8, XC_SEG_SS = 0xCC;
static const uint32_t XC_SIZE = 716;
static const uint32_t XR_CODE = 0x00, XR_FLAGS = 0x04, XR_RECORD = 0x08, XR_ADDR = 0x0C;
static const uint32_t XR_NPARAMS = 0x10, XR_INFO = 0x18, XR_SIZE = 80;

static void seh_fill_context(x86_context* ctx, uint32_t c) {
    x86_mem_write32(ctx, c + XC_CONTEXT_FLAGS, 0x0001003F); // CONTROL|INTEGER|SEGMENTS
    x86_mem_write32(ctx, c + XC_SEG_GS, ctx->regs.gs);
    x86_mem_write32(ctx, c + XC_SEG_FS, ctx->regs.fs);
    x86_mem_write32(ctx, c + XC_EDI, ctx->regs.edi);
    x86_mem_write32(ctx, c + XC_ESI, ctx->regs.esi);
    x86_mem_write32(ctx, c + XC_EBX, ctx->regs.ebx);
    x86_mem_write32(ctx, c + XC_EDX, ctx->regs.edx);
    x86_mem_write32(ctx, c + XC_ECX, ctx->regs.ecx);
    x86_mem_write32(ctx, c + XC_EAX, ctx->regs.eax);
    x86_mem_write32(ctx, c + XC_EBP, ctx->regs.ebp);
    x86_mem_write32(ctx, c + XC_EIP, ctx->regs.eip);
    x86_mem_write32(ctx, c + XC_SEG_CS, ctx->regs.cs);
    x86_mem_write32(ctx, c + XC_EFLAGS, ctx->regs.eflags);
    x86_mem_write32(ctx, c + XC_ESP, ctx->regs.esp);
    x86_mem_write32(ctx, c + XC_SEG_SS, ctx->regs.ss);
}

int x86_raise_seh(x86_context* ctx, uint32_t code, uint32_t flags,
                  uint32_t addr, uint32_t nparams, const uint32_t* params) {
    if (!ctx->regs.fs_base || !ctx->seh_sentinel) return -1;

    // ExceptionList is the first dword of the TEB; FS:[0] holds its head.
    uint32_t rec = x86_mem_read32(ctx, ctx->regs.fs_base);
    if (rec == 0 || rec == 0xFFFFFFFF) return -1;

    uint32_t slot = (ctx->regs.esp - XC_SIZE - XR_SIZE - 16u) & ~15u;
    uint32_t ctxrec = slot;
    uint32_t exrec = slot + XC_SIZE;
    for (uint32_t i = 0; i < XC_SIZE + XR_SIZE; i += 4)
        x86_mem_write32(ctx, slot + i, 0);

    x86_mem_write32(ctx, exrec + XR_CODE, code);
    x86_mem_write32(ctx, exrec + XR_FLAGS, flags);
    x86_mem_write32(ctx, exrec + XR_RECORD, 0);
    x86_mem_write32(ctx, exrec + XR_ADDR, addr);
    uint32_t n = nparams > 15 ? 15 : nparams;
    x86_mem_write32(ctx, exrec + XR_NPARAMS, n);
    for (uint32_t i = 0; i < n; i++)
        x86_mem_write32(ctx, exrec + XR_INFO + i * 4, params ? params[i] : 0);

    for (int depth = 0; depth < 32 && rec != 0 && rec != 0xFFFFFFFF; depth++) {
        uint32_t prev = x86_mem_read32(ctx, rec);
        uint32_t handler = x86_mem_read32(ctx, rec + 4);
        if (!handler) break;

        seh_fill_context(ctx, ctxrec);
        uint32_t args[4] = { exrec, rec, ctxrec, 0 };
        uint32_t r = x86_call_guest(ctx, handler, args, 4);
        if (r == 0) { // ExceptionContinueExecution
            ctx->regs.edi = x86_mem_read32(ctx, ctxrec + XC_EDI);
            ctx->regs.esi = x86_mem_read32(ctx, ctxrec + XC_ESI);
            ctx->regs.ebx = x86_mem_read32(ctx, ctxrec + XC_EBX);
            ctx->regs.edx = x86_mem_read32(ctx, ctxrec + XC_EDX);
            ctx->regs.ecx = x86_mem_read32(ctx, ctxrec + XC_ECX);
            ctx->regs.eax = x86_mem_read32(ctx, ctxrec + XC_EAX);
            ctx->regs.ebp = x86_mem_read32(ctx, ctxrec + XC_EBP);
            ctx->regs.eip = x86_mem_read32(ctx, ctxrec + XC_EIP);
            ctx->regs.eflags = x86_mem_read32(ctx, ctxrec + XC_EFLAGS);
            ctx->regs.esp = x86_mem_read32(ctx, ctxrec + XC_ESP);
            return 0;
        }
        rec = prev; // ExceptionContinueSearch
    }
    return -1;
}

void x86_halt(x86_context* ctx, uint32_t address) {
    ctx->running = 0;
    if (ctx->image_size && address != ctx->seh_sentinel) {
        char line[112];
        snprintf(line, sizeof(line), "x86: privileged HLT at guest address 0x%08X", address);
        if (ctx->log) ctx->log(line);
        ctx->execution = X86_EXEC_BAD_INSTRUCTION;
    }
}

int x86_step(x86_context* ctx) {
    if (!ctx->running) return -1;
    static const bool trace_insn = getenv("TRT_X86_TRACE_I") != nullptr;
    if (trace_insn) {
        const u32 e = ctx->regs.eip;
        auto peek = [ctx](u32 address) -> unsigned {
            return ctx->mem.base && x86_page_mapped(ctx, address >> X86_PAGE_SHIFT)
                ? ctx->mem.base[address] : 0; // tracing never commits an unmapped page
        };
        fprintf(stderr, "I %08X %02X %02X %02X %02X %02X %02X  esp=%08X eax=%08X ecx=%08X\n",
                e, peek(e), peek(e + 1), peek(e + 2), peek(e + 3), peek(e + 4), peek(e + 5),
                ctx->regs.esp, ctx->regs.eax, ctx->regs.ecx);
    }
    if (ctx->insn_count >= ctx->insn_limit) {
        if (ctx->log) ctx->log("x86: instruction limit reached");
        ctx->running = 0;
        if (ctx->execution == X86_EXEC_OK)
            ctx->execution = X86_EXEC_INSTRUCTION_LIMIT;
        return -1;
    }

    // A call into the import trap range is an API call, not an instruction.
    {
        int t = x86_trap_dispatch(ctx);
        if (t != 0) return t < 0 ? -1 : 0;
    }

    // Executing from an uncommitted page is an access violation. Deliver it to
    // the guest's SEH chain before failing.
    if (!x86_page_mapped(ctx, ctx->regs.eip >> X86_PAGE_SHIFT)) {
        uint32_t info[2] = { 0, ctx->regs.eip }; // 0 = read (no write to report)
        if (x86_raise_seh(ctx, 0xC0000005u, 0, ctx->regs.eip, 2, info) == 0) return 0;
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "x86: unhandled access violation executing 0x%08X (insn #%llu, no SEH handler)",
                 ctx->regs.eip, (unsigned long long)ctx->insn_count);
        if (ctx->log) ctx->log(buf);
        ctx->running = 0;
        ctx->execution = X86_EXEC_ACCESS_VIOLATION;
        return -1;
    }

    uint32_t eip_start = ctx->regs.eip;
    uint8_t op = x86_mem_read8(ctx, ctx->regs.eip++);

    // Prefixes
    g_seg_override = -1;
    int operand_size = 32; // default 32-bit
    int addr_size = 32;
    int rep = 0;
    (void)operand_size; (void)addr_size; (void)rep;

    while (op == 0x66 || op == 0x67 || op == 0xF3 || op == 0xF2 ||
           op == 0xF0 ||
           op == 0x26 || op == 0x2E || op == 0x36 || op == 0x3E || op == 0x64 || op == 0x65) {
        // 0xF0 LOCK: the guest is single-threaded here, so the atomicity the
        // prefix buys is already true. Consume it and run the instruction.
        if (op == 0x66) operand_size = 16;
        if (op == 0x67) addr_size = 16;
        if (op == 0xF3) rep = 1;
        if (op == 0xF2) rep = 2;
        if (op == 0x26) g_seg_override = 0; // ES
        if (op == 0x2E) g_seg_override = 1; // CS
        if (op == 0x36) g_seg_override = 2; // SS
        if (op == 0x3E) g_seg_override = 3; // DS
        if (op == 0x64) g_seg_override = 4; // FS
        if (op == 0x65) g_seg_override = 5; // GS
        op = x86_mem_read8(ctx, ctx->regs.eip++);
    }

    ctx->insn_count++;

    // ---- NOP ----
    if (op == 0x90) {
        return 0;
    }

    // ---- HLT ----
    if (op == 0xF4) {
        x86_halt(ctx, eip_start);
        return 0;
    }

    // ---- CLI / STI (0xFA / 0xFB) ----
    // The guest never receives a hardware interrupt here, so the interrupt
    // flag is bookkeeping only: set/clear it and continue.
    if (op == 0xFA) { ctx->regs.eflags &= ~(uint32_t)X86_IF; return 0; }
    if (op == 0xFB) { ctx->regs.eflags |= (uint32_t)X86_IF; return 0; }

    // ---- 0xFF group ----
    if (op == 0xFF) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        int ext = modrm_reg(modrm); // extension opcode
        if (ext == 0 || ext == 1) { // INC / DEC r/m32 (CF is not touched)
            uint32_t val, addr = 0;
            uint32_t* dst;
            if (modrm_mod(modrm) == 3) {
                dst = x86_reg_ptr(ctx, modrm_rm(modrm));
                val = *dst;
            } else {
                int consumed = 0;
                addr = calc_ea(ctx, modrm, &consumed);
                ctx->regs.eip += consumed - 1;
                dst = nullptr;
                val = x86_mem_read32(ctx, addr);
            }
            uint32_t cf = ctx->regs.eflags & X86_CF;
            uint32_t result = (ext == 0) ? val + 1 : val - 1;
            // Both INC and DEC raise OF/SF/ZF/AF/PF and leave CF alone. The
            // add/sub helpers compute SF as the sign bit of the result, which
            // is exactly what the CPU does for both.
            if (ext == 0) x86_update_flags_add(ctx, val, 1, result);
            else          x86_update_flags_sub(ctx, val, 1, result);
            ctx->regs.eflags = (ctx->regs.eflags & ~X86_CF) | cf;
            if (dst) *dst = result;
            else x86_mem_write32(ctx, addr, result);
            return 0;
        }
        if (ext == 6) { // PUSH r/m32
            uint32_t val;
            if (modrm_mod(modrm) == 3) {
                val = *x86_reg_ptr(ctx, modrm_rm(modrm));
            } else {
                int consumed = 0;
                uint32_t addr = calc_ea(ctx, modrm, &consumed);
                ctx->regs.eip += consumed - 1;
                val = x86_mem_read32(ctx, addr);
            }
            ctx->regs.esp -= 4;
            x86_mem_write32(ctx, ctx->regs.esp, val);
            return 0;
        }
        if (ext == 2 || ext == 4) { // near CALL / near JMP
            uint32_t target;
            if (modrm_mod(modrm) == 3) {
                target = *x86_reg_ptr(ctx, modrm_rm(modrm));
            } else {
                int consumed = 0;
                uint32_t addr = calc_ea(ctx, modrm, &consumed);
                ctx->regs.eip += consumed - 1;
                target = x86_mem_read32(ctx, addr);
            }
            if (ext == 2) {
                ctx->regs.esp -= 4;
                x86_mem_write32(ctx, ctx->regs.esp, ctx->regs.eip);
            }
            ctx->regs.eip = target;
            return 0;
        }
        // Far transfers change CS and are unsupported in this flat guest.
        char message[96];
        snprintf(message, sizeof(message), "x86: unsupported FF /%d at EIP=0x%08X",
                 ext, eip_start);
        if (ctx->log) ctx->log(message);
        ctx->running = 0;
        ctx->execution = X86_EXEC_BAD_INSTRUCTION;
        return -1;
    }
    // ---- POP r/m32 (0x8F /0) ----
    if (op == 0x8F) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        uint32_t val = x86_mem_read32(ctx, ctx->regs.esp);
        ctx->regs.esp += 4;
        if (modrm_mod(modrm) == 3) {
            *x86_reg_ptr(ctx, modrm_rm(modrm)) = val;
        } else {
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            x86_mem_write32(ctx, addr, val);
        }
        return 0;
    }

    // ---- RET (near) ----
    if (op == 0xC3) {
        ctx->regs.eip = x86_mem_read32(ctx, ctx->regs.esp);
        ctx->regs.esp += 4;
        return 0;
    }
    if (op == 0xC2) {
        uint16_t imm = x86_mem_read16(ctx, ctx->regs.eip);
        ctx->regs.eip += 2;
        ctx->regs.eip = x86_mem_read32(ctx, ctx->regs.esp);
        ctx->regs.esp += 4 + imm;
        return 0;
    }

    // ---- INT 3 (breakpoint) ----
    if (op == 0xCC) {
        uint32_t info = eip_start;
        if (x86_raise_seh(ctx, 0x80000003u, 0, eip_start, 1, &info) == 0) return 0;
        if (ctx->log) ctx->log("x86: INT3 breakpoint (unhandled: no SEH handler)");
        ctx->running = 0;
        ctx->execution = X86_EXEC_BAD_INSTRUCTION;
        return -1;
    }

    // ---- STC / CLC / CMC (0xF9 / 0xF8 / 0xF5) ----
    if (op == 0xF9) { ctx->regs.eflags |= X86_CF; return 0; }
    if (op == 0xF8) { ctx->regs.eflags &= ~X86_CF; return 0; }
    if (op == 0xF5) { ctx->regs.eflags ^= X86_CF; return 0; }

    // ---- INT imm8 (0xCD) ----
    // 0xCD 0x03 is a breakpoint; any other vector (0x29 = MSVC __fastfail, the
    // /GS stack-cookie and __report_gsfailure path) terminates the process in
    // user mode, so stop there and say which vector it was.
    if (op == 0xCD) {
        uint8_t vec = x86_mem_read8(ctx, ctx->regs.eip++);
        if (vec == 3) {
            uint32_t info = eip_start;
            if (x86_raise_seh(ctx, 0x80000003u, 0, eip_start, 1, &info) == 0) return 0;
            if (ctx->log) ctx->log("x86: INT3 breakpoint (unhandled: no SEH handler)");
            ctx->running = 0;
            ctx->execution = X86_EXEC_BAD_INSTRUCTION;
            return -1;
        }
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "x86: INT 0x%02X at EIP=0x%08X -> guest asked for termination "
                 "(0x29 = __fastfail; no Win32 user-mode handler exists)",
                 vec, eip_start);
        if (ctx->log) ctx->log(buf);
        ctx->running = 0;
        ctx->execution = X86_EXEC_UNHANDLED_TRAP;
        return -1;
    }

    // ---- PUSH (register) ----
    if (op >= 0x50 && op <= 0x57) {
        int reg = op - 0x50;
        ctx->regs.esp -= 4;
        x86_mem_write32(ctx, ctx->regs.esp, *x86_reg_ptr(ctx, reg));
        return 0;
    }

    // ---- POP (register) ----
    if (op >= 0x58 && op <= 0x5F) {
        int reg = op - 0x58;
        *x86_reg_ptr(ctx, reg) = x86_mem_read32(ctx, ctx->regs.esp);
        ctx->regs.esp += 4;
        return 0;
    }

    // ---- PUSH imm32 ----
    if (op == 0x68) {
        uint32_t imm = read_imm32(ctx);
        ctx->regs.esp -= 4;
        x86_mem_write32(ctx, ctx->regs.esp, imm);
        return 0;
    }

    // ---- PUSH imm8 (sign-extended) ----
    if (op == 0x6A) {
        int8_t imm = (int8_t)read_imm8(ctx);
        ctx->regs.esp -= 4;
        x86_mem_write32(ctx, ctx->regs.esp, (uint32_t)imm);
        return 0;
    }

    // ---- CALL rel32 ----
    if (op == 0xE8) {
        int32_t rel = (int32_t)read_imm32(ctx);
        ctx->regs.esp -= 4;
        x86_mem_write32(ctx, ctx->regs.esp, ctx->regs.eip);
        ctx->regs.eip += rel;
        return 0;
    }

    // ---- JMP rel32 ----
    if (op == 0xE9) {
        int32_t rel = (int32_t)read_imm32(ctx);
        ctx->regs.eip += rel;
        return 0;
    }

    // ---- JMP rel8 ----
    if (op == 0xEB) {
        int8_t rel = (int8_t)read_imm8(ctx);
        ctx->regs.eip += rel;
        return 0;
    }

    // ---- Short conditional jumps (0x70-0x7F) ----
    if (op >= 0x70 && op <= 0x7F) {
        int8_t rel = (int8_t)read_imm8(ctx);
        if (x86_condition(ctx->regs.eflags, op - 0x70)) {
            ctx->regs.eip += rel;
        }
        return 0;
    }

    // ---- 0x0F prefixed instructions ----
    if (op == 0x0F) {
        uint8_t op2 = x86_mem_read8(ctx, ctx->regs.eip++);
        // Conditional jumps (0x80-0x8F)
        if (op2 >= 0x80 && op2 <= 0x8F) {
            int32_t rel = (int32_t)read_imm32(ctx);
            if (x86_condition(ctx->regs.eflags, op2 - 0x80)) {
                ctx->regs.eip += rel;
            }
            return 0;
        }
        // IMUL r16/32, r/m16/32: CF and OF report signed truncation.
        if (op2 == 0xAF) {
            const uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
            const bool narrow = operand_size == 16;
            uint32_t source;
            if (modrm_mod(modrm) == 3) {
                source = *x86_reg_ptr(ctx, modrm_rm(modrm));
            } else {
                int consumed = 0;
                const uint32_t address = calc_ea(ctx, modrm, &consumed);
                ctx->regs.eip += consumed - 1;
                source = narrow ? x86_mem_read16(ctx, address) : x86_mem_read32(ctx, address);
            }
            auto& dest = *x86_reg_ptr(ctx, modrm_reg(modrm));
            const int64_t product = narrow
                ? int64_t(int16_t(dest)) * int16_t(source)
                : int64_t(int32_t(dest)) * int32_t(source);
            const int64_t truncated = narrow ? int64_t(int16_t(product)) : int64_t(int32_t(product));
            dest = narrow ? (dest & 0xFFFF0000u) | uint16_t(product) : uint32_t(product);
            ctx->regs.eflags &= ~(X86_CF | X86_OF);
            if (product != truncated) ctx->regs.eflags |= X86_CF | X86_OF;
            return 0;
        }
        // MOVZX (0xB6 = byte, 0xB7 = word)
        if (op2 == 0xB6) {
            uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
            if (modrm_mod(modrm) == 3) {
                uint32_t val = read_r8(ctx, modrm_rm(modrm));
                *x86_reg_ptr(ctx, modrm_reg(modrm)) = val;
            } else {
                int consumed = 0;
                uint32_t addr = calc_ea(ctx, modrm, &consumed);
                ctx->regs.eip += consumed - 1;
                uint32_t val = x86_mem_read8(ctx, addr);
                *x86_reg_ptr(ctx, modrm_reg(modrm)) = val;
            }
            return 0;
        }
        if (op2 == 0xB7) {
            uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
            if (modrm_mod(modrm) == 3) {
                uint32_t val = *x86_reg_ptr(ctx, modrm_rm(modrm)) & 0xFFFF;
                *x86_reg_ptr(ctx, modrm_reg(modrm)) = val;
            } else {
                int consumed = 0;
                uint32_t addr = calc_ea(ctx, modrm, &consumed);
                ctx->regs.eip += consumed - 1;
                uint32_t val = x86_mem_read16(ctx, addr);
                *x86_reg_ptr(ctx, modrm_reg(modrm)) = val;
            }
            return 0;
        }
        // MOVSX (0xBE = byte, 0xBF = word)
        if (op2 == 0xBE) {
            uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
            uint32_t val;
            if (modrm_mod(modrm) == 3) {
                val = (uint32_t)(int32_t)(int8_t)read_r8(ctx, modrm_rm(modrm));
            } else {
                int consumed = 0;
                uint32_t addr = calc_ea(ctx, modrm, &consumed);
                ctx->regs.eip += consumed - 1;
                val = (int32_t)(int8_t)x86_mem_read8(ctx, addr);
            }
            *x86_reg_ptr(ctx, modrm_reg(modrm)) = val;
            return 0;
        }
        if (op2 == 0xBF) {
            uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
            uint32_t val;
            if (modrm_mod(modrm) == 3) {
                val = (uint32_t)(int32_t)(int16_t)(*x86_reg_ptr(ctx, modrm_rm(modrm)) & 0xFFFF);
            } else {
                int consumed = 0;
                uint32_t addr = calc_ea(ctx, modrm, &consumed);
                ctx->regs.eip += consumed - 1;
                val = (int32_t)(int16_t)x86_mem_read16(ctx, addr);
            }
            *x86_reg_ptr(ctx, modrm_reg(modrm)) = val;
            return 0;
        }
        // ---- CPUID (0x0F 0xA2) ----
        if (op2 == 0xA2) {
            uint32_t leaf = ctx->regs.eax;
            uint32_t sub = ctx->regs.ecx;
            uint32_t a = 0, b = 0, c = 0, d = 0;
            if (leaf == 0) {
                a = 0x0000000D; // max basic leaf
                b = 0x756E6547; // "Genu"
                d = 0x49656E69; // "ineI"
                c = 0x6C65746E; // "ntel"
            } else if (leaf == 1) {
                a = 0x000306A9; // family 6, model 58, stepping 9
                // EDX: FPU(0) VME(1) DE(2) PSE(3) TSC(4) MSR(5) PAE(6) MCE(7)
                //      CX8(8) APIC(9) SEP(11) MTRR(12) PGE(13) MCA(14) CMOV(15)
                //      PAT(16) PSE36(17) CLFSH(19) MMX(23) FXSR(24) SSE(25) SSE2(26) SS(27)
                d = (1u<<0)|(1u<<3)|(1u<<4)|(1u<<5)|(1u<<6)|(1u<<8)|(1u<<11)|(1u<<13)|
                    (1u<<15)|(1u<<19)|(1u<<23)|(1u<<24)|(1u<<25)|(1u<<26)|(1u<<27);
                // ECX: SSE3(0) PCLMUL(1) DTES64(2) MONITOR(3) DS-CPL(4) VMX(5) SMX(6)
                //      EIST(7) TM2(8) SSSE3(9) CNXT-ID(10) FMA(12) CX16(13) SSE4.1(19)
                //      SSE4.2(20) x2APIC(21) MOVBE(22) POPCNT(23) AES(25) XSAVE(26)
                c = (1u<<0)|(1u<<9)|(1u<<13)|(1u<<19)|(1u<<22)|(1u<<23)|(1u<<26);
                b = 0x00000800; // logical processors
            } else if (leaf == 0x80000000) {
                a = 0x80000008;
            } else if (leaf == 0x80000001) {
                // EDX: SYSCALL(11) XD(20) MMX/SSE already implied
                d = (1u<<11)|(1u<<20)|(1u<<29); // + LM bit? do not claim long mode
                c = 0;
            } else if (leaf == 0x80000008) {
                a = 0x2028; // 40-bit phys, 32-bit linear
            } else if (leaf == 4 || leaf == 0x0B || leaf == 0x0D) {
                a = 0; // topology/xsave: none
            } else {
                a = b = c = d = 0;
            }
            (void)sub;
            ctx->regs.eax = a;
            ctx->regs.ebx = b;
            ctx->regs.ecx = c;
            ctx->regs.edx = d;
            return 0;
        }
        // ---- RDTSC (0x0F 0x31) ----
        if (op2 == 0x31) {
            static uint64_t rd_tsc = 0x100000;
            rd_tsc += 3000; // ~3GHz-ish progression per read
            ctx->regs.eax = (uint32_t)(rd_tsc & 0xFFFFFFFFu);
            ctx->regs.edx = (uint32_t)(rd_tsc >> 32);
            return 0;
        }
        // ---- RDMSR (0x0F 0x32) / WRMSR (0x0F 0x30) - safe stubs ----
        if (op2 == 0x32) {
            ctx->regs.eax = 0;
            ctx->regs.edx = 0;
            return 0;
        }
        if (op2 == 0x30) {
            return 0;
        }
        // ---- SYSENTER (0x0F 0x34) / SYSEXIT (0x0F 0x35) ----
        if (op2 == 0x34) {
            // Route to the NT syscall bridge: EDX holds the call index.
            // Without a real syscall table, fail loudly in the log and halt.
            if (ctx->log) ctx->log("x86: SYSENTER unsupported - halting");
            ctx->running = 0;
            return -1;
        }
        if (op2 == 0x35) {
            return 0;
        }

        // ---- FXSAVE / FXRSTOR / fences / CLFLUSH (0x0F 0xAE) ----
        if (op2 == 0xAE) {
            uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
            int mod = modrm_mod(modrm);
            int ext = modrm_reg(modrm);
            if (mod == 3) {
                // LFENCE (/5), MFENCE (/6), SFENCE (/7) - ordering only, no-op
                if (ext >= 5) return 0;
                return 0;
            }
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            if (ext == 0) { // FXSAVE
                x86_mem_write16(ctx, addr + 0, ctx->regs.fpu_cw);
                x86_mem_write16(ctx, addr + 2, ctx->regs.fpu_sw);
                x86_mem_write8(ctx, addr + 4, 0); // ftw: not tracked precisely
                x86_mem_write16(ctx, addr + 6, 0);
                x86_mem_write32(ctx, addr + 8, 0);
                x86_mem_write32(ctx, addr + 12, 0);
                x86_mem_write32(ctx, addr + 16, 0);
                x86_mem_write32(ctx, addr + 20, 0);
                x86_mem_write32(ctx, addr + 24, ctx->regs.mxcsr ? ctx->regs.mxcsr : 0x1F80);
                x86_mem_write32(ctx, addr + 28, 0xFFFF);
                for (int i = 0; i < 8; i++) {
                    int idx = (ctx->regs.fpu_top + i) & 7;
                    double v = ctx->regs.fpu[idx];
                    uint64_t raw; memcpy(&raw, &v, 8);
                    xg_write64(ctx, addr + 32 + i * 16, raw);
                    x86_mem_write16(ctx, addr + 32 + i * 16 + 8, 0xFFFF);
                    xg_write64(ctx, addr + 32 + i * 16 + 8, 0);
                }
                for (int i = 0; i < 8; i++) xg_write(ctx, addr + 160 + i * 16, ctx->regs.xmm[i], 16);
                return 0;
            }
            if (ext == 1) { // FXRSTOR
                ctx->regs.fpu_cw = x86_mem_read16(ctx, addr + 0);
                ctx->regs.fpu_sw = x86_mem_read16(ctx, addr + 2);
                ctx->regs.mxcsr = x86_mem_read32(ctx, addr + 24);
                for (int i = 0; i < 8; i++) {
                    uint64_t raw = xg_read64(ctx, addr + 32 + i * 16);
                    double v; memcpy(&v, &raw, 8);
                    int idx = (ctx->regs.fpu_top + i) & 7;
                    ctx->regs.fpu[idx] = v;
                    ctx->regs.fpu_tags[idx] = 0;
                }
                for (int i = 0; i < 8; i++) xg_read(ctx, addr + 160 + i * 16, ctx->regs.xmm[i], 16);
                return 0;
            }
            if (ext == 7) { // CLFLUSH
                return 0;
            }
            return 0;
        }

        // ================= SSE / SSE2 / MMX =================
        {
            int p66 = (operand_size == 16);

            // ---- MOVUPS / MOVUPD / MOVSS / MOVSD (0x10 load, 0x11 store) ----
            if (op2 == 0x10 || op2 == 0x11) {
                uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                int dst = modrm_reg(modrm);
                uint8_t buf[16] = {0};
                int nbytes = (rep == 1) ? 4 : (rep == 2) ? 8 : 16;
                if (op2 == 0x10) { sse_fetch(ctx, modrm, buf); memcpy(ctx->regs.xmm[dst], buf, nbytes); }
                else { memcpy(buf, ctx->regs.xmm[dst], nbytes); sse_store_n(ctx, modrm, buf, nbytes); }
                return 0;
            }
            // ---- MOVAPS / MOVAPD (0x28 load, 0x29 store) ----
            if (op2 == 0x28 || op2 == 0x29) {
                uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                int dst = modrm_reg(modrm);
                uint8_t buf[16];
                if (op2 == 0x28) { sse_fetch(ctx, modrm, buf); memcpy(ctx->regs.xmm[dst], buf, 16); }
                else { memcpy(buf, ctx->regs.xmm[dst], 16); sse_store(ctx, modrm, buf); }
                return 0;
            }
            // ---- MOVNTPS / MOVNTPD (0x2B store) ----
            if (op2 == 0x2B) {
                uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                uint8_t buf[16];
                memcpy(buf, ctx->regs.xmm[modrm_reg(modrm)], 16);
                sse_store(ctx, modrm, buf);
                return 0;
            }
            // ---- MOVDQA / MOVDQU / MOVQ (0x6F load, 0x7F store) ----
            if (op2 == 0x6F || op2 == 0x7F) {
                uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                int dst = modrm_reg(modrm);
                if (p66 || rep == 1) { // MOVDQA (66) / MOVDQU (F3): 16 bytes
                    uint8_t buf[16];
                    if (op2 == 0x6F) { sse_fetch(ctx, modrm, buf); memcpy(ctx->regs.xmm[dst], buf, 16); }
                    else { memcpy(buf, ctx->regs.xmm[dst], 16); sse_store(ctx, modrm, buf); }
                } else { // MOVQ mm: 8 bytes
                    uint8_t buf[16] = {0};
                    if (op2 == 0x6F) { sse_fetch64(ctx, modrm, 1, buf); memcpy(&ctx->regs.mm[dst], buf, 8); }
                    else { memcpy(buf, &ctx->regs.mm[dst], 8); sse_store64(ctx, modrm, 1, buf); }
                }
                return 0;
            }
            // ---- MOVD / MOVQ (0x6E load, 0x7E store) ----
            if (op2 == 0x6E) {
                uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                int dst = modrm_reg(modrm);
                if (rep == 1) { // MOVQ xmm, r/m64
                    uint8_t buf[16] = {0};
                    sse_fetch64(ctx, modrm, 0, buf);
                    memcpy(ctx->regs.xmm[dst], buf, 8);
                } else if (p66) { // MOVD xmm, r/m32
                    uint32_t v;
                    if (modrm_mod(modrm) == 3) v = *x86_reg_ptr(ctx, modrm_rm(modrm));
                    else { int c = 0; uint32_t a = calc_ea(ctx, modrm, &c); ctx->regs.eip += c - 1; v = x86_mem_read32(ctx, a); }
                    memset(ctx->regs.xmm[dst], 0, 16);
                    memcpy(ctx->regs.xmm[dst], &v, 4);
                } else { // MOVD mm, r/m32
                    uint32_t v;
                    if (modrm_mod(modrm) == 3) v = *x86_reg_ptr(ctx, modrm_rm(modrm));
                    else { int c = 0; uint32_t a = calc_ea(ctx, modrm, &c); ctx->regs.eip += c - 1; v = x86_mem_read32(ctx, a); }
                    ctx->regs.mm[dst] = v;
                }
                return 0;
            }
            if (op2 == 0x7E) {
                uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                int src = modrm_reg(modrm);
                if (rep == 1) { // MOVQ r/m64, xmm
                    uint8_t buf[16] = {0};
                    memcpy(buf, ctx->regs.xmm[src], 8);
                    sse_store64(ctx, modrm, 0, buf);
                } else if (p66) { // MOVD r/m32, xmm
                    uint32_t v; memcpy(&v, ctx->regs.xmm[src], 4);
                    if (modrm_mod(modrm) == 3) *x86_reg_ptr(ctx, modrm_rm(modrm)) = v;
                    else { int c = 0; uint32_t a = calc_ea(ctx, modrm, &c); ctx->regs.eip += c - 1; x86_mem_write32(ctx, a, v); }
                } else { // MOVD r/m32, mm
                    uint32_t v = (uint32_t)ctx->regs.mm[src];
                    if (modrm_mod(modrm) == 3) *x86_reg_ptr(ctx, modrm_rm(modrm)) = v;
                    else { int c = 0; uint32_t a = calc_ea(ctx, modrm, &c); ctx->regs.eip += c - 1; x86_mem_write32(ctx, a, v); }
                }
                return 0;
            }
            // ---- MOVQ r/m64, xmm (0xD6 with 66) ----
            if (op2 == 0xD6 && p66) {
                uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                uint8_t buf[16] = {0};
                memcpy(buf, ctx->regs.xmm[modrm_reg(modrm)], 8);
                sse_store64(ctx, modrm, 0, buf);
                return 0;
            }
            // ---- MOVNTDQ (0xE7 with 66) ----
            if (op2 == 0xE7 && p66) {
                uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                uint8_t buf[16];
                memcpy(buf, ctx->regs.xmm[modrm_reg(modrm)], 16);
                sse_store(ctx, modrm, buf);
                return 0;
            }
            // ---- Bitwise packed single/double: 0x54 AND, 0x55 ANDN,
            //      0x56 OR, 0x57 XOR (prefix-independent: these are bitwise) ----
            if (op2 == 0x54 || op2 == 0x55 || op2 == 0x56 || op2 == 0x57) {
                uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                int dst = modrm_reg(modrm);
                uint8_t src[16] = {0};
                sse_fetch(ctx, modrm, src);
                for (int i = 0; i < 16; i++) {
                    switch (op2) {
                        case 0x54: ctx->regs.xmm[dst][i] &= src[i]; break;
                        case 0x55: ctx->regs.xmm[dst][i] = (uint8_t)(~ctx->regs.xmm[dst][i] & src[i]); break;
                        case 0x56: ctx->regs.xmm[dst][i] |= src[i]; break;
                        default:   ctx->regs.xmm[dst][i] ^= src[i]; break;
                    }
                }
                return 0;
            }
            // ---- Packed/scalar arithmetic: 0x51 sqrt, 0x58 add, 0x59 mul,
            //      0x5C sub, 0x5D min, 0x5E div, 0x5F max ----
            if (op2 == 0x51 || op2 == 0x58 || op2 == 0x59 || op2 == 0x5C ||
                op2 == 0x5D || op2 == 0x5E || op2 == 0x5F) {
                uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                int dst = modrm_reg(modrm);
                uint8_t src[16] = {0};
                sse_fetch(ctx, modrm, src);
                int aop = (op2 == 0x58) ? VA_ADD : (op2 == 0x59) ? VA_MUL :
                          (op2 == 0x5C) ? VA_SUB : (op2 == 0x5E) ? VA_DIV :
                          (op2 == 0x51) ? VA_SQRT : (op2 == 0x5D) ? VA_MIN : VA_MAX;
                if (rep == 2) sse_arith_f64(aop, ctx->regs.xmm[dst], src, 1);      // scalar double
                else if (p66) sse_arith_f64(aop, ctx->regs.xmm[dst], src, 2);      // packed double
                else if (rep == 1) sse_arith_f32(aop, ctx->regs.xmm[dst], src, 1); // scalar single
                else sse_arith_f32(aop, ctx->regs.xmm[dst], src, 4);               // packed single
                return 0;
            }
            // ---- Convert 0x5A: CVTPS2PD / CVTPD2PS / CVTSS2SD / CVTSD2SS ----
            if (op2 == 0x5A) {
                uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                int dst = modrm_reg(modrm);
                uint8_t src[16] = {0};
                sse_fetch(ctx, modrm, src);
                uint8_t out[16] = {0};
                if (rep == 1) { float f; memcpy(&f, src, 4); double d = (double)f; memcpy(out, &d, 8); }
                else if (rep == 2) { double d; memcpy(&d, src, 8); float f = (float)d; memcpy(out, &f, 4); }
                else if (p66) { double d[2]; memcpy(d, src, 16); float f[2] = {(float)d[0], (float)d[1]}; memcpy(out, f, 8); }
                else { float f[2]; memcpy(f, src, 8); double d[2] = {(double)f[0], (double)f[1]}; memcpy(out, d, 16); }
                memcpy(ctx->regs.xmm[dst], out, 16);
                return 0;
            }
            // ---- Convert 0x5B: CVTDQ2PS / CVTPS2DQ / CVTTPS2DQ ----
            if (op2 == 0x5B) {
                uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                int dst = modrm_reg(modrm);
                uint8_t src[16] = {0};
                sse_fetch(ctx, modrm, src);
                uint8_t out[16] = {0};
                if (p66) { float f[4]; memcpy(f, src, 16); int32_t i[4] = {(int32_t)f[0],(int32_t)f[1],(int32_t)f[2],(int32_t)f[3]}; memcpy(out, i, 16); }
                else if (rep == 1) { float f[4]; memcpy(f, src, 16); int32_t i[4] = {(int32_t)f[0],(int32_t)f[1],(int32_t)f[2],(int32_t)f[3]}; memcpy(out, i, 16); }
                else { int32_t i[4]; memcpy(i, src, 16); float f[4] = {(float)i[0],(float)i[1],(float)i[2],(float)i[3]}; memcpy(out, f, 16); }
                memcpy(ctx->regs.xmm[dst], out, 16);
                return 0;
            }
            // ---- Convert 0xE6: CVTTPD2DQ / CVTPD2DQ / CVTDQ2PD ----
            if (op2 == 0xE6 && (p66 || rep)) {
                uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                int dst = modrm_reg(modrm);
                uint8_t src[16] = {0};
                sse_fetch(ctx, modrm, src);
                uint8_t out[16] = {0};
                if (p66) { double d[2]; memcpy(d, src, 16); int32_t i[2] = {(int32_t)d[0], (int32_t)d[1]}; memcpy(out, i, 8); }
                else if (rep == 2) { double d[2]; memcpy(d, src, 16); int32_t i[2] = {(int32_t)d[0], (int32_t)d[1]}; memcpy(out, i, 8); }
                else { int32_t i[2]; memcpy(i, src, 8); double d[2] = {(double)i[0], (double)i[1]}; memcpy(out, d, 16); }
                memcpy(ctx->regs.xmm[dst], out, 16);
                return 0;
            }
            // ---- CVTSI2SS / CVTSI2SD / CVTPI2PS / CVTPI2PD (0x2A) ----
            if (op2 == 0x2A) {
                uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                int dst = modrm_reg(modrm);
                int32_t iv;
                if (modrm_mod(modrm) == 3) iv = (int32_t)*x86_reg_ptr(ctx, modrm_rm(modrm));
                else { int c = 0; uint32_t a = calc_ea(ctx, modrm, &c); ctx->regs.eip += c - 1; iv = (int32_t)x86_mem_read32(ctx, a); }
                if (rep == 2) { double d = (double)iv; memcpy(ctx->regs.xmm[dst], &d, 8); }
                else if (p66) { float f[4] = {(float)iv, 0, 0, 0}; memcpy(ctx->regs.xmm[dst], f, 16); }
                else if (rep == 1) { float f = (float)iv; memcpy(ctx->regs.xmm[dst], &f, 4); }
                else { ctx->regs.mm[dst] = (uint64_t)(uint32_t)iv; }
                return 0;
            }
            // ---- CVTTSS2SI/CVTTSD2SI (0x2C), CVTSS2SI/CVTSD2SI (0x2D) ----
            if (op2 == 0x2C || op2 == 0x2D) {
                uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                uint8_t src[16] = {0};
                sse_fetch(ctx, modrm, src);
                int32_t out;
                if (p66 && (op2 == 0x2C || op2 == 0x2D)) { double d; memcpy(&d, src, 8); out = (int32_t)d; }
                else if (rep == 2) { double d; memcpy(&d, src, 8); out = (int32_t)d; }
                else if (rep == 1) { float f; memcpy(&f, src, 4); out = (int32_t)f; }
                else { double d; memcpy(&d, src, 8); out = (int32_t)d; }
                *x86_reg_ptr(ctx, modrm_reg(modrm)) = (uint32_t)out;
                return 0;
            }
            // ---- COMISS/UCOMISS/COMISD/UCOMISD (0x2E, 0x2F) ----
            if (op2 == 0x2E || op2 == 0x2F) {
                uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                uint8_t src[16] = {0};
                sse_fetch(ctx, modrm, src);
                double a, b;
                if (p66 || rep == 2) { memcpy(&a, ctx->regs.xmm[modrm_reg(modrm)], 8); memcpy(&b, src, 8); }
                else { float fa, fb; memcpy(&fa, ctx->regs.xmm[modrm_reg(modrm)], 4); memcpy(&fb, src, 4); a = fa; b = fb; }
                ctx->regs.eflags &= ~(X86_ZF | X86_CF | X86_PF | X86_OF | X86_SF | X86_AF);
                if (a > b) { }
                else if (a < b) ctx->regs.eflags |= X86_CF;
                else if (a == b) ctx->regs.eflags |= X86_ZF;
                else { ctx->regs.eflags |= (X86_ZF | X86_CF | X86_PF); }
                return 0;
            }
            // ---- MOVMSKPS / MOVMSKPD (0x50) ----
            if (op2 == 0x50 && (p66 || !rep)) {
                uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                int src = modrm_rm(modrm);
                uint32_t mask = 0;
                if (p66) { for (int i = 0; i < 2; i++) mask |= (uint32_t)((ctx->regs.xmm[src][i * 8 + 7] & 0x80) ? (1u << i) : 0); }
                else { for (int i = 0; i < 4; i++) mask |= (uint32_t)((ctx->regs.xmm[src][i * 4 + 3] & 0x80) ? (1u << i) : 0); }
                *x86_reg_ptr(ctx, modrm_reg(modrm)) = mask;
                return 0;
            }

            // ---- Multi-byte NOP and prefetch hints (0x0F 0x18-0x1F) ----
            if (op2 >= 0x18 && op2 <= 0x1F) {
                uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                if (modrm_mod(modrm) != 3) {
                    int c = 0;
                    (void)calc_ea(ctx, modrm, &c);
                    ctx->regs.eip += c - 1;
                }
                return 0;
            }
            // ---- SHUFPS / SHUFPD (0x0F 0xC6) ----
            if (op2 == 0xC6) {
                uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                int dst = modrm_reg(modrm);
                int imm = x86_mem_read8(ctx, ctx->regs.eip++);
                uint8_t src[16] = {0};
                sse_fetch(ctx, modrm, src);
                uint8_t d[16];
                memcpy(d, ctx->regs.xmm[dst], 16);
                uint8_t out[16] = {0};
                if (p66) { // SHUFPD
                    int s0 = imm & 1, s1 = (imm >> 1) & 1;
                    memcpy(out + 0, d + (s0 ? 8 : 0), 8);
                    memcpy(out + 8, src + (s1 ? 8 : 0), 8);
                } else { // SHUFPS
                    uint32_t dv[4], sv[4], ov[4];
                    memcpy(dv, d, 16); memcpy(sv, src, 16);
                    ov[0] = dv[imm & 3];
                    ov[1] = dv[(imm >> 2) & 3];
                    ov[2] = sv[(imm >> 4) & 3];
                    ov[3] = sv[(imm >> 6) & 3];
                    memcpy(out, ov, 16);
                }
                memcpy(ctx->regs.xmm[dst], out, 16);
                return 0;
            }
            // ---- UNPCKLPS/PD (0x14), UNPCKHPS/PD (0x15) ----
            if (op2 == 0x14 || op2 == 0x15) {
                uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                int dst = modrm_reg(modrm);
                uint8_t src[16] = {0};
                sse_fetch(ctx, modrm, src);
                uint8_t d[16];
                memcpy(d, ctx->regs.xmm[dst], 16);
                uint8_t out[16] = {0};
                if (p66) { // doubles: 2 lanes
                    if (op2 == 0x14) { memcpy(out, d, 8); memcpy(out + 8, src, 8); }
                    else { memcpy(out, d + 8, 8); memcpy(out + 8, src + 8, 8); }
                } else { // singles: 4 lanes
                    if (op2 == 0x14) {
                        memcpy(out + 0, d + 0, 4); memcpy(out + 4, src + 0, 4);
                        memcpy(out + 8, d + 4, 4); memcpy(out + 12, src + 4, 4);
                    } else {
                        memcpy(out + 0, d + 8, 4); memcpy(out + 4, src + 8, 4);
                        memcpy(out + 8, d + 12, 4); memcpy(out + 12, src + 12, 4);
                    }
                }
                memcpy(ctx->regs.xmm[dst], out, 16);
                return 0;
            }
            // ---- MOVLPS / MOVHPS / MOVLPD / MOVHPD (0x12 load, 0x13 store) ----
            if (op2 == 0x12 || op2 == 0x13) {
                uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                int dst = modrm_reg(modrm);
                int high = (op2 == 0x13) ? 1 : 0;
                if (modrm_mod(modrm) == 3) {
                    // MOVHLPS/MOVLHPS register form
                    uint8_t src[16];
                    memcpy(src, ctx->regs.xmm[modrm_rm(modrm)], 16);
                    uint8_t out[16];
                    memcpy(out, ctx->regs.xmm[dst], 16);
                    if (op2 == 0x12) memcpy(out, src + 8, 8);      // MOVHLPS
                    else memcpy(out + 8, src, 8);                    // MOVLHPS
                    memcpy(ctx->regs.xmm[dst], out, 16);
                    return 0;
                }
                int c = 0;
                uint32_t addr = calc_ea(ctx, modrm, &c);
                ctx->regs.eip += c - 1;
                uint8_t buf[16] = {0};
                if (op2 == 0x12) {
                    xg_read(ctx, addr, buf, 8);
                    memcpy(ctx->regs.xmm[dst], buf, 8);
                } else {
                    memcpy(buf, ctx->regs.xmm[dst] + 8, 8);
                    xg_write(ctx, addr, buf, 8);
                }
                (void)high;
                return 0;
            }
            // ---- PUNPCKLQDQ / PUNPCKHQDQ (0x0F 0x6C/0x6D with 66) ----
            if ((op2 == 0x6C || op2 == 0x6D) && p66) {
                uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                int dst = modrm_reg(modrm);
                uint8_t src[16] = {0};
                sse_fetch(ctx, modrm, src);
                uint8_t out[16];
                if (op2 == 0x6C) { memcpy(out, ctx->regs.xmm[dst], 8); memcpy(out + 8, src, 8); }
                else { memcpy(out, ctx->regs.xmm[dst] + 8, 8); memcpy(out + 8, src + 8, 8); }
                memcpy(ctx->regs.xmm[dst], out, 16);
                return 0;
            }
            // ---- CMPPS / CMPSS / CMPPD / CMPSD (0x0F 0xC2 + imm8) ----
            if (op2 == 0xC2) {
                uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                int dst = modrm_reg(modrm);
                int imm = x86_mem_read8(ctx, ctx->regs.eip++);
                uint8_t src[16] = {0};
                sse_fetch(ctx, modrm, src);
                int lanes = (rep == 1 || rep == 2) ? 1 : (p66 ? 2 : 4);
                int dbl = (p66 || rep == 2);
                uint8_t out[16] = {0};
                memcpy(out, ctx->regs.xmm[dst], 16);
                for (int i = 0; i < lanes; i++) {
                    double a, b;
                    if (dbl) { memcpy(&a, ctx->regs.xmm[dst] + i * 8, 8); memcpy(&b, src + i * 8, 8); }
                    else { float fa, fb; memcpy(&fa, ctx->regs.xmm[dst] + i * 4, 4); memcpy(&fb, src + i * 4, 4); a = fa; b = fb; }
                    int r = 0;
                    switch (imm & 7) {
                        case 0: r = (a == b); break; // EQ
                        case 1: r = (a < b); break;  // LT
                        case 2: r = (a <= b); break; // LE
                        case 3: r = (a != a || b != b); break; // UNORD
                        case 4: r = !(a == b); break; // NEQ
                        case 5: r = !(a < b); break;  // NLT
                        case 6: r = !(a <= b); break; // NLE
                        default: r = (a == a && b == b); break; // ORD
                    }
                    if (dbl) { uint64_t m = r ? ~0ull : 0ull; memcpy(out + i * 8, &m, 8); }
                    else { uint32_t m = r ? 0xFFFFFFFFu : 0u; memcpy(out + i * 4, &m, 4); }
                }
                memcpy(ctx->regs.xmm[dst], out, 16);
                return 0;
            }

            // ---- Integer SIMD (MMX when no prefix, XMM with 66) ----
            {
                int use_mmx = !p66;
                int elems8 = use_mmx ? 8 : 16;
                int elems16 = use_mmx ? 4 : 8;
                int elems32 = use_mmx ? 2 : 4;
                // opcode -> (op, element bytes)
                int op = -1, esz = 1;
                switch (op2) {
                    case 0xFC: op = VADD; esz = 1; break;
                    case 0xFD: op = VADD; esz = 2; break;
                    case 0xFE: op = VADD; esz = 4; break;
                    case 0xF8: op = VSUB; esz = 1; break;
                    case 0xF9: op = VSUB; esz = 2; break;
                    case 0xFA: op = VSUB; esz = 4; break;
                    case 0xDB: op = VAND; esz = 8; break;
                    case 0xEB: op = VOR;  esz = 8; break;
                    case 0xEF: op = VXOR; esz = 8; break;
                    case 0x74: op = VEQ;  esz = 1; break;
                    case 0x75: op = VEQ;  esz = 2; break;
                    case 0x76: op = VEQ;  esz = 4; break;
                    case 0x64: op = VGT;  esz = 1; break;
                    case 0x65: op = VGT;  esz = 2; break;
                    case 0x66: op = VGT;  esz = 4; break;
                    case 0xDA: op = VMINU; esz = 1; break;
                    case 0xDE: op = VMAXU; esz = 1; break;
                    case 0xE0: op = VAVG;  esz = 1; break;
                    default: break;
                }
                if (op >= 0) {
                    uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                    int dst = modrm_reg(modrm);
                    uint8_t src[16] = {0};
                    sse_fetch_ivec(ctx, modrm, use_mmx, src);
                    uint8_t d[16] = {0};
                    if (use_mmx) memcpy(d, &ctx->regs.mm[dst], 8); else memcpy(d, ctx->regs.xmm[dst], 16);
                    if (esz == 1) vec_op8(op, d, src, elems8);
                    else if (esz == 2) vec_op16(op, d, src, elems16);
                    else if (esz == 4) vec_op32(op, d, src, elems32);
                    else { // 64-bit logical
                        uint64_t a, b; memcpy(&a, d, 8); memcpy(&b, src, 8);
                        uint64_t r = (op == VAND) ? (a & b) : (op == VOR) ? (a | b) : (a ^ b);
                        memcpy(d, &r, 8);
                    }
                    if (use_mmx) memcpy(&ctx->regs.mm[dst], d, 8); else memcpy(ctx->regs.xmm[dst], d, 16);
                    return 0;
                }
                // PANDN (0xDF): d = (~d) & src
                if (op2 == 0xDF) {
                    uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                    int dst = modrm_reg(modrm);
                    uint8_t src[16] = {0};
                    sse_fetch_ivec(ctx, modrm, use_mmx, src);
                    uint8_t d[16] = {0};
                    if (use_mmx) memcpy(d, &ctx->regs.mm[dst], 8); else memcpy(d, ctx->regs.xmm[dst], 16);
                    for (int i = 0; i < (use_mmx ? 8 : 16); i++) d[i] = (uint8_t)(~d[i] & src[i]);
                    if (use_mmx) memcpy(&ctx->regs.mm[dst], d, 8); else memcpy(ctx->regs.xmm[dst], d, 16);
                    return 0;
                }
                // PUNPCKL/H (0x60-0x62, 0x68-0x6A)
                if ((op2 >= 0x60 && op2 <= 0x62) || (op2 >= 0x68 && op2 <= 0x6A)) {
                    uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                    int dst = modrm_reg(modrm);
                    uint8_t src[16] = {0};
                    sse_fetch_ivec(ctx, modrm, use_mmx, src);
                    uint8_t d[16] = {0};
                    if (use_mmx) memcpy(d, &ctx->regs.mm[dst], 8); else memcpy(d, ctx->regs.xmm[dst], 16);
                    int units = (op2 == 0x60 || op2 == 0x68) ? 1 : (op2 == 0x61 || op2 == 0x69) ? 2 : 4;
                    int lo = (op2 <= 0x62);
                    uint8_t out[16] = {0};
                    int count = (use_mmx ? 8 : 16) / units;
                    for (int i = 0; i < count / 2; i++) {
                        int didx = lo ? i : (count / 2 + i);
                        memcpy(out + (i * 2) * units, d + didx * units, units);
                        memcpy(out + (i * 2 + 1) * units, src + didx * units, units);
                    }
                    if (use_mmx) memcpy(&ctx->regs.mm[dst], out, 8); else memcpy(ctx->regs.xmm[dst], out, 16);
                    return 0;
                }
                // PACKSSWB/PACKSSDW/PACKUSWB (0x63, 0x6B, 0x67)
                if (op2 == 0x63 || op2 == 0x6B || op2 == 0x67) {
                    uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                    int dst = modrm_reg(modrm);
                    uint8_t src[16] = {0};
                    sse_fetch_ivec(ctx, modrm, use_mmx, src);
                    uint8_t d[16] = {0};
                    if (use_mmx) memcpy(d, &ctx->regs.mm[dst], 8); else memcpy(d, ctx->regs.xmm[dst], 16);
                    int fmt = (use_mmx ? 8 : 16);
                    uint8_t out[16] = {0};
                    if (op2 == 0x63) { // pack 16->8 signed
                        for (int i = 0; i < fmt / 2; i++) {
                            int16_t a; memcpy(&a, d + i * 2, 2);
                            int16_t b; memcpy(&b, src + i * 2, 2);
                            out[i] = (uint8_t)(a < -128 ? -128 : a > 127 ? 127 : a);
                            out[fmt / 2 + i] = (uint8_t)(b < -128 ? -128 : b > 127 ? 127 : b);
                        }
                    } else if (op2 == 0x67) { // pack 16->8 unsigned
                        for (int i = 0; i < fmt / 2; i++) {
                            int16_t a; memcpy(&a, d + i * 2, 2);
                            int16_t b; memcpy(&b, src + i * 2, 2);
                            out[i] = (uint8_t)(a < 0 ? 0 : a > 255 ? 255 : a);
                            out[fmt / 2 + i] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
                        }
                    } else { // pack 32->16 signed
                        for (int i = 0; i < fmt / 4; i++) {
                            int32_t a; memcpy(&a, d + i * 4, 4);
                            int32_t b; memcpy(&b, src + i * 4, 4);
                            int16_t r1 = (int16_t)(a < -32768 ? -32768 : a > 32767 ? 32767 : a);
                            int16_t r2 = (int16_t)(b < -32768 ? -32768 : b > 32767 ? 32767 : b);
                            memcpy(out + i * 2, &r1, 2);
                            memcpy(out + (fmt / 2) + i * 2, &r2, 2);
                        }
                    }
                    if (use_mmx) memcpy(&ctx->regs.mm[dst], out, 8); else memcpy(ctx->regs.xmm[dst], out, 16);
                    return 0;
                }
                // Shifts by register: 0xD1/D2/D3 (right), 0xE1/E2 (right arith), 0xF1/F2/F3 (left)
                if (op2 == 0xD1 || op2 == 0xD2 || op2 == 0xD3 ||
                    op2 == 0xE1 || op2 == 0xE2 ||
                    op2 == 0xF1 || op2 == 0xF2 || op2 == 0xF3) {
                    uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                    int dst = modrm_reg(modrm);
                    uint8_t src[16] = {0};
                    sse_fetch_ivec(ctx, modrm, use_mmx, src);
                    uint64_t cnt64; memcpy(&cnt64, src, 8);
                    int count = (int)(cnt64 & 0xFF);
                    uint8_t d[16] = {0};
                    if (use_mmx) memcpy(d, &ctx->regs.mm[dst], 8); else memcpy(d, ctx->regs.xmm[dst], 16);
                    if (op2 == 0xD1 || op2 == 0xE1 || op2 == 0xF1) sse_shift16(d, count, op2 == 0xF1, op2 == 0xE1, elems16);
                    else if (op2 == 0xD2 || op2 == 0xE2 || op2 == 0xF2) sse_shift32(d, count, op2 == 0xF2, op2 == 0xE2, elems32);
                    else sse_shift64(d, count, op2 == 0xF3);
                    if (use_mmx) memcpy(&ctx->regs.mm[dst], d, 8); else memcpy(ctx->regs.xmm[dst], d, 16);
                    return 0;
                }
                // Shifts by immediate: 0x71 (16-bit), 0x72 (32-bit), 0x73 (64-bit)
                if (op2 == 0x71 || op2 == 0x72 || op2 == 0x73) {
                    uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                    int dst = modrm_rm(modrm);
                    int ext = modrm_reg(modrm);
                    int count = x86_mem_read8(ctx, ctx->regs.eip++);
                    uint8_t d[16] = {0};
                    if (use_mmx) memcpy(d, &ctx->regs.mm[dst], 8); else memcpy(d, ctx->regs.xmm[dst], 16);
                    if (op2 == 0x71) {
                        if (ext == 2) sse_shift16(d, count, 0, 0, elems16);
                        else if (ext == 4) sse_shift16(d, count, 0, 1, elems16);
                        else if (ext == 6) sse_shift16(d, count, 1, 0, elems16);
                    } else if (op2 == 0x72) {
                        if (ext == 2) sse_shift32(d, count, 0, 0, elems32);
                        else if (ext == 4) sse_shift32(d, count, 0, 1, elems32);
                        else if (ext == 6) sse_shift32(d, count, 1, 0, elems32);
                    } else {
                        if (ext == 2) sse_shift64(d, count, 0);
                        else if (ext == 6) sse_shift64(d, count, 1);
                    }
                    if (use_mmx) memcpy(&ctx->regs.mm[dst], d, 8); else memcpy(ctx->regs.xmm[dst], d, 16);
                    return 0;
                }
                // PMOVMSKB (0xD7)
                if (op2 == 0xD7) {
                    uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                    int src = modrm_rm(modrm);
                    uint8_t v[16] = {0};
                    if (use_mmx) memcpy(v, &ctx->regs.mm[src], 8); else memcpy(v, ctx->regs.xmm[src], 16);
                    uint32_t mask = 0;
                    int n = use_mmx ? 8 : 16;
                    for (int i = 0; i < n; i++) if (v[i] & 0x80) mask |= (1u << i);
                    *x86_reg_ptr(ctx, modrm_reg(modrm)) = mask;
                    return 0;
                }
                // PSHUFW/PSHUFD/PSHUFLW/PSHUFHW (0x70)
                if (op2 == 0x70) {
                    uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
                    int dst = modrm_reg(modrm);
                    int imm = x86_mem_read8(ctx, ctx->regs.eip++);
                    uint8_t src[16] = {0};
                    sse_fetch(ctx, modrm, src);
                    uint8_t out[16] = {0};
                    if (rep == 2) { // PSHUFLW
                        for (int i = 0; i < 4; i++) { int s = (imm >> (i * 2)) & 3; memcpy(out + i * 2, src + s * 2, 2); }
                        memcpy(out + 8, src + 8, 8);
                    } else if (rep == 1) { // PSHUFHW
                        memcpy(out, src, 8);
                        for (int i = 0; i < 4; i++) { int s = (imm >> (i * 2)) & 3; memcpy(out + 8 + i * 2, src + 8 + s * 2, 2); }
                    } else if (p66) { // PSHUFD
                        for (int i = 0; i < 4; i++) { int s = (imm >> (i * 2)) & 3; memcpy(out + i * 4, src + s * 4, 4); }
                    } else { // PSHUFW (MMX)
                        for (int i = 0; i < 4; i++) { int s = (imm >> (i * 2)) & 3; memcpy(out + i * 2, src + s * 2, 2); }
                    }
                    if (use_mmx) memcpy(&ctx->regs.mm[dst], out, 8); else memcpy(ctx->regs.xmm[dst], out, 16);
                    return 0;
                }
            }
            // ---- EMMS (0x77) ----
            if (op2 == 0x77 && !p66) {
                memset(ctx->regs.mm, 0, sizeof(ctx->regs.mm));
                return 0;
            }
        }

        // ---- CMOVcc (0x0F 0x40-0x4F) ----
        if (op2 >= 0x40 && op2 <= 0x4F) {
            uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
            int cc = op2 - 0x40;
            uint32_t src_val;
            if (modrm_mod(modrm) == 3) {
                src_val = *x86_reg_ptr(ctx, modrm_rm(modrm));
            } else {
                int consumed = 0;
                uint32_t addr = calc_ea(ctx, modrm, &consumed);
                ctx->regs.eip += consumed - 1;
                src_val = x86_mem_read32(ctx, addr);
            }
            if (x86_condition(ctx->regs.eflags, cc)) {
                *x86_reg_ptr(ctx, modrm_reg(modrm)) = src_val;
            }
            return 0;
        }
        // ---- SETcc (0x0F 0x90-0x9F) ----
        if (op2 >= 0x90 && op2 <= 0x9F) {
            uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
            int cc = op2 - 0x90;
            uint8_t val = x86_condition(ctx->regs.eflags, cc) ? 1 : 0;
            if (modrm_mod(modrm) == 3) {
                // Write to low byte of register
                uint32_t* reg = x86_reg_ptr(ctx, modrm_rm(modrm));
                *reg = (*reg & 0xFFFFFF00) | val;
            } else {
                int consumed = 0;
                uint32_t addr = calc_ea(ctx, modrm, &consumed);
                ctx->regs.eip += consumed - 1;
                x86_mem_write8(ctx, addr, val);
            }
            return 0;
        }
        // ---- BSF (0xBC): bit scan forward ----
        if (op2 == 0xBC) {
            uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
            uint32_t src_val;
            if (modrm_mod(modrm) == 3) {
                src_val = *x86_reg_ptr(ctx, modrm_rm(modrm));
            } else {
                int consumed = 0;
                uint32_t addr = calc_ea(ctx, modrm, &consumed);
                ctx->regs.eip += consumed - 1;
                src_val = x86_mem_read32(ctx, addr);
            }
            if (src_val == 0) {
                ctx->regs.eflags |= X86_ZF;
            } else {
                ctx->regs.eflags &= ~X86_ZF;
                int idx = 0;
                while (!(src_val & (1 << idx))) idx++;
                *x86_reg_ptr(ctx, modrm_reg(modrm)) = idx;
            }
            return 0;
        }
        // ---- BSR (0xBD): bit scan reverse ----
        if (op2 == 0xBD) {
            uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
            uint32_t src_val;
            if (modrm_mod(modrm) == 3) {
                src_val = *x86_reg_ptr(ctx, modrm_rm(modrm));
            } else {
                int consumed = 0;
                uint32_t addr = calc_ea(ctx, modrm, &consumed);
                ctx->regs.eip += consumed - 1;
                src_val = x86_mem_read32(ctx, addr);
            }
            if (src_val == 0) {
                ctx->regs.eflags |= X86_ZF;
            } else {
                ctx->regs.eflags &= ~X86_ZF;
                int idx = 31;
                while (!(src_val & (1u << idx))) idx--;
                *x86_reg_ptr(ctx, modrm_reg(modrm)) = idx;
            }
            return 0;
        }
        // BT/BTS/BTR/BTC. A memory operand with a register index is a
        // bit string: a signed index can select a word before or after base.
        if (op2 == 0xBA || op2 == 0xA3 || op2 == 0xAB || op2 == 0xB3 || op2 == 0xBB) {
            const uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
            const bool immediate = op2 == 0xBA;
            const int operation = immediate ? modrm_reg(modrm) - 4 : (op2 - 0xA3) / 8;
            if (operation < 0) {
                if (ctx->log) ctx->log("x86: invalid 0F BA bit-test extension");
                ctx->running = 0;
                return -1;
            }
            const bool narrow = operand_size == 16;
            const bool is_reg = modrm_mod(modrm) == 3;
            const unsigned width = narrow ? 16 : 32;
            uint32_t addr = 0;
            if (!is_reg) {
                int consumed = 0;
                addr = calc_ea(ctx, modrm, &consumed);
                ctx->regs.eip += consumed - 1;
            }
            int32_t index;
            if (immediate) {
                index = int32_t(read_imm8(ctx));
            } else {
                const auto src = *x86_reg_ptr(ctx, modrm_reg(modrm));
                index = narrow ? int16_t(src) : int32_t(src);
            }
            const unsigned bit = uint32_t(index) & (width - 1);
            if (!is_reg && !immediate)
                addr += uint32_t(((index - int32_t(bit)) / int32_t(width)) * int32_t(width / 8));
            auto* reg = is_reg ? x86_reg_ptr(ctx, modrm_rm(modrm)) : nullptr;
            uint32_t value = reg ? *reg : narrow ? x86_mem_read16(ctx, addr) : x86_mem_read32(ctx, addr);
            const uint32_t mask = uint32_t{1} << bit;
            ctx->regs.eflags = (ctx->regs.eflags & ~X86_CF) | (value & mask ? X86_CF : 0);
            if (operation == 0) return 0;
            if (operation == 1) value |= mask;
            else if (operation == 2) value &= ~mask;
            else value ^= mask;
            if (reg) *reg = value;
            else if (narrow) x86_mem_write16(ctx, addr, uint16_t(value));
            else x86_mem_write32(ctx, addr, value);
            return 0;
        }
        // ---- BSWAP (0xC8-0xCF): byte swap ----
        if (op2 >= 0xC8 && op2 <= 0xCF) {
            int reg = op2 - 0xC8;
            uint32_t val = *x86_reg_ptr(ctx, reg);
            *x86_reg_ptr(ctx, reg) = ((val & 0xFF) << 24) | ((val & 0xFF00) << 8) |
                                     ((val & 0xFF0000) >> 8) | ((val & 0xFF000000) >> 24);
            return 0;
        }
        // ---- XADD (0xC0/0xC1): exchange and add ----
        if (op2 == 0xC0 || op2 == 0xC1) {
            uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
            int size = (op2 == 0xC1) ? 4 : 1;
            if (modrm_mod(modrm) == 3) {
                if (size == 4) {
                    uint32_t* reg1 = x86_reg_ptr(ctx, modrm_reg(modrm));
                    uint32_t* reg2 = x86_reg_ptr(ctx, modrm_rm(modrm));
                    uint32_t tmp = *reg1;
                    *reg1 += *reg2;
                    *reg2 = tmp;
                    x86_update_flags_logic(ctx, *reg1);
                }
            } else {
                int consumed = 0;
                uint32_t addr = calc_ea(ctx, modrm, &consumed);
                ctx->regs.eip += consumed - 1;
                if (size == 4) {
                    uint32_t mem = x86_mem_read32(ctx, addr);
                    uint32_t reg = *x86_reg_ptr(ctx, modrm_reg(modrm));
                    x86_mem_write32(ctx, addr, mem + reg);
                    *x86_reg_ptr(ctx, modrm_reg(modrm)) = mem;
                    x86_update_flags_logic(ctx, mem + reg);
                }
            }
            return 0;
        }

        // ---- SHLD (0xA4=imm8, 0xA5=CL) ----
        if (op2 == 0xA4 || op2 == 0xA5) {
            uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
            uint8_t count;
            if (op2 == 0xA4) {
                count = x86_mem_read8(ctx, ctx->regs.eip++);
            } else {
                count = ctx->regs.ecx & 0x1F;
            }
            uint32_t src_val;
            if (modrm_mod(modrm) == 3) {
                src_val = *x86_reg_ptr(ctx, modrm_rm(modrm));
                uint32_t* dst = x86_reg_ptr(ctx, modrm_reg(modrm));
                if (count == 0) { /* no change */ }
                else if (count < 32) {
                    uint64_t combined = ((uint64_t)*dst << 32) | src_val;
                    *dst = (uint32_t)(combined >> (32 - count));
                    // flags
                    ctx->regs.eflags = (ctx->regs.eflags & ~X86_CF) | ((*dst >> (count - 1)) & 1 ? X86_CF : 0);
                    if (count == 1) {
                        // OF handling simplified
                    }
                }
            } else {
                int consumed = 0;
                uint32_t addr = calc_ea(ctx, modrm, &consumed);
                ctx->regs.eip += consumed - 1;
                src_val = x86_mem_read32(ctx, addr); // simplified
            }
            return 0;
        }
        // ---- SHRD (0xAC=imm8, 0xAD=CL) ----
        if (op2 == 0xAC || op2 == 0xAD) {
            uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
            uint8_t count;
            if (op2 == 0xAC) {
                count = x86_mem_read8(ctx, ctx->regs.eip++);
            } else {
                count = ctx->regs.ecx & 0x1F;
            }
            if (modrm_mod(modrm) == 3) {
                uint32_t src_val = *x86_reg_ptr(ctx, modrm_rm(modrm));
                uint32_t* dst = x86_reg_ptr(ctx, modrm_reg(modrm));
                if (count > 0 && count < 32) {
                    uint64_t combined = ((uint64_t)src_val << 32) | *dst;
                    *dst = (uint32_t)(combined >> count);
                    ctx->regs.eflags = (ctx->regs.eflags & ~X86_CF) | ((*dst >> (count - 1)) & 1 ? 0 : X86_CF); // simplified
                }
            }
            return 0;
        }
        // ---- CMPXCHG (0xB0=byte, 0xB1=dword) ----
        if (op2 == 0xB0 || op2 == 0xB1) {
            uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
            if (op2 == 0xB1) {
                if (modrm_mod(modrm) == 3) {
                    uint32_t* reg = x86_reg_ptr(ctx, modrm_rm(modrm));
                    uint32_t* acc = &ctx->regs.eax;
                    if (*acc == *reg) {
                        ctx->regs.eflags |= X86_ZF;
                        *reg = *x86_reg_ptr(ctx, modrm_reg(modrm));
                    } else {
                        ctx->regs.eflags &= ~X86_ZF;
                        *acc = *reg;
                    }
                } else {
                    int consumed = 0;
                    uint32_t addr = calc_ea(ctx, modrm, &consumed);
                    ctx->regs.eip += consumed - 1;
                    uint32_t mem = x86_mem_read32(ctx, addr);
                    if (ctx->regs.eax == mem) {
                        ctx->regs.eflags |= X86_ZF;
                        x86_mem_write32(ctx, addr, *x86_reg_ptr(ctx, modrm_reg(modrm)));
                    } else {
                        ctx->regs.eflags &= ~X86_ZF;
                        ctx->regs.eax = mem;
                    }
                }
            }
            return 0;
        }

        // Unknown 0x0F instruction
        char buf[128];
        snprintf(buf, sizeof(buf), "x86: unhandled 0x0F 0x%02X at EIP=0x%08X", op2, eip_start);
        if (ctx->log) ctx->log(buf);
        ctx->running = 0;
        return -1;
    }

    // ---- MOV r32, r/m32 (0x8B) ----
    if (op == 0x8B) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        if (modrm_mod(modrm) == 3) {
            *x86_reg_ptr(ctx, modrm_reg(modrm)) = *x86_reg_ptr(ctx, modrm_rm(modrm));
        } else {
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            *x86_reg_ptr(ctx, modrm_reg(modrm)) = x86_mem_read32(ctx, addr);
        }
        return 0;
    }

    // ---- MOV r/m32, r32 (0x89) ----
    if (op == 0x89) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        if (modrm_mod(modrm) == 3) {
            *x86_reg_ptr(ctx, modrm_rm(modrm)) = *x86_reg_ptr(ctx, modrm_reg(modrm));
        } else {
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            x86_mem_write32(ctx, addr, *x86_reg_ptr(ctx, modrm_reg(modrm)));
        }
        return 0;
    }

    // ---- MOV r/m8, r8 (0x88) ----
    if (op == 0x88) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        uint8_t val = (uint8_t)(*x86_reg_ptr(ctx, modrm_reg(modrm)) & 0xFF);
        if (modrm_mod(modrm) == 3) {
            *x86_reg_ptr(ctx, modrm_rm(modrm)) = (*x86_reg_ptr(ctx, modrm_rm(modrm)) & 0xFFFFFF00) | val;
        } else {
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            x86_mem_write8(ctx, addr, val);
        }
        return 0;
    }

    // ---- MOV r8, r/m8 (0x8A) ----
    if (op == 0x8A) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        uint8_t val;
        if (modrm_mod(modrm) == 3) {
            val = (uint8_t)(*x86_reg_ptr(ctx, modrm_rm(modrm)) & 0xFF);
        } else {
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            val = x86_mem_read8(ctx, addr);
        }
        *x86_reg_ptr(ctx, modrm_reg(modrm)) = (*x86_reg_ptr(ctx, modrm_reg(modrm)) & 0xFFFFFF00) | val;
        return 0;
    }

    // ---- MOV AL/EAX, moffs (0xA0/0xA1) and MOV moffs, AL/EAX (0xA2/0xA3) ----
    // Absolute-addressed forms the compiler emits for globals: OpenTTD's CRT
    // entry hits 0xA1 (`mov eax, ds:0x11bacc0`) right after the QPC probe.
    if (op >= 0xA0 && op <= 0xA3) {
        uint32_t addr = read_imm32(ctx);
        if (g_seg_override == 4) addr += ctx->regs.fs_base;
        if (op == 0xA0) {
            uint32_t v = x86_mem_read8(ctx, addr);
            ctx->regs.eax = (ctx->regs.eax & 0xFFFFFF00) | v;
        } else if (op == 0xA1) {
            ctx->regs.eax = x86_mem_read32(ctx, addr);
        } else if (op == 0xA2) {
            x86_mem_write8(ctx, addr, (uint8_t)(ctx->regs.eax & 0xFF));
        } else {
            x86_mem_write32(ctx, addr, ctx->regs.eax);
        }
        return 0;
    }

    // ---- MOV r8, imm8 (0xB0-0xB7) ----
    // The 8-bit immediate form, no ModRM: 0xB0-0xB3 address AL/CL/DL/BL and
    // 0xB4-0xB7 AH/CH/DH/BH (the high-byte registers 0xC6 /0 cannot reach).
    // OpenTTD's PE32 CRT entry stops at `mov al, 1`, so this is load-bearing.
    if (op >= 0xB0 && op <= 0xB7) {
        *r8p(ctx, op - 0xB0) = read_imm8(ctx);
        return 0;
    }

    // MOV register, immediate (B8+r) and MOV r/m, immediate (C6/C7).
    if (op >= 0xB8 && op <= 0xBF) {
        auto* reg = x86_reg_ptr(ctx, op - 0xB8);
        if (operand_size == 16) *reg = (*reg & 0xFFFF0000u) | read_imm16(ctx);
        else *reg = read_imm32(ctx);
        return 0;
    }
    if (op == 0xC6 || op == 0xC7) {
        const uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        if (modrm_reg(modrm) != 0) {
            if (ctx->log) ctx->log("x86: unsupported MOV immediate extension");
            ctx->running = 0;
            return -1;
        }
        const bool byte = op == 0xC6;
        const bool word = operand_size == 16;
        const bool is_reg = modrm_mod(modrm) == 3;
        uint32_t address = 0;
        if (!is_reg) {
            int consumed = 0;
            address = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
        }
        const uint32_t immediate = byte ? read_imm8(ctx)
            : word ? read_imm16(ctx) : read_imm32(ctx);
        if (is_reg) {
            const int index = modrm_rm(modrm);
            if (byte) *r8p(ctx, index) = uint8_t(immediate);
            else {
                auto* reg = x86_reg_ptr(ctx, index);
                *reg = word ? (*reg & 0xFFFF0000u) | immediate : immediate;
            }
        } else if (byte) x86_mem_write8(ctx, address, uint8_t(immediate));
        else if (word) x86_mem_write16(ctx, address, uint16_t(immediate));
        else x86_mem_write32(ctx, address, immediate);
        return 0;
    }

    // ---- ADD r/m32, r32 (0x01) ----
    if (op == 0x01) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        uint32_t src = *x86_reg_ptr(ctx, modrm_reg(modrm));
        if (modrm_mod(modrm) == 3) {
            uint32_t* dst = x86_reg_ptr(ctx, modrm_rm(modrm));
            uint32_t result = *dst + src;
            x86_update_flags_add(ctx, *dst, src, result);
            *dst = result;
        } else {
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            uint32_t dst = x86_mem_read32(ctx, addr);
            uint32_t result = dst + src;
            x86_update_flags_add(ctx, dst, src, result);
            x86_mem_write32(ctx, addr, result);
        }
        return 0;
    }

    // ---- ADD r32, r/m32 (0x03) ----
    if (op == 0x03) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        uint32_t src;
        if (modrm_mod(modrm) == 3) {
            src = *x86_reg_ptr(ctx, modrm_rm(modrm));
        } else {
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            src = x86_mem_read32(ctx, addr);
        }
        uint32_t* dst = x86_reg_ptr(ctx, modrm_reg(modrm));
        uint32_t result = *dst + src;
        x86_update_flags_add(ctx, *dst, src, result);
        *dst = result;
        return 0;
    }

    // ---- ADD AL/AX/EAX, imm (0x05) ----
    if (op == 0x05) {
        uint32_t imm = read_imm32(ctx);
        uint32_t result = ctx->regs.eax + imm;
        x86_update_flags_add(ctx, ctx->regs.eax, imm, result);
        ctx->regs.eax = result;
        return 0;
    }

    // ADC/SBB r/m,reg and reg,r/m for all operand widths.
    if ((op & 0xF4) == 0x10) {
        const uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        const bool byte = !(op & 1);
        const unsigned width = byte ? 8 : operand_size;
        const bool reg_dest = (op & 2) != 0;
        const bool is_reg = modrm_mod(modrm) == 3;
        uint32_t address = 0;
        if (!is_reg) {
            int consumed = 0;
            address = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
        }
        const int reg = modrm_reg(modrm), rm = modrm_rm(modrm);
        const auto reg_value = byte ? read_r8(ctx, reg) : *x86_reg_ptr(ctx, reg);
        const auto rm_value = is_reg ? (byte ? read_r8(ctx, rm) : *x86_reg_ptr(ctx, rm))
            : byte ? x86_mem_read8(ctx, address)
            : width == 16 ? x86_mem_read16(ctx, address) : x86_mem_read32(ctx, address);
        const auto result = alu_with_carry(ctx, reg_dest ? reg_value : rm_value,
            reg_dest ? rm_value : reg_value, width, (op & 8) != 0);
        if (reg_dest || is_reg) {
            const int dest = reg_dest ? reg : rm;
            if (byte) *r8p(ctx, dest) = uint8_t(result);
            else {
                auto* ptr = x86_reg_ptr(ctx, dest);
                *ptr = width == 16 ? (*ptr & 0xFFFF0000u) | result : result;
            }
        } else if (byte) x86_mem_write8(ctx, address, uint8_t(result));
        else if (width == 16) x86_mem_write16(ctx, address, uint16_t(result));
        else x86_mem_write32(ctx, address, result);
        return 0;
    }

    // ---- SUB r/m32, r32 (0x29) ----
    if (op == 0x29) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        uint32_t src = *x86_reg_ptr(ctx, modrm_reg(modrm));
        if (modrm_mod(modrm) == 3) {
            uint32_t* dst = x86_reg_ptr(ctx, modrm_rm(modrm));
            uint32_t result = *dst - src;
            x86_update_flags_sub(ctx, *dst, src, result);
            *dst = result;
        } else {
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            uint32_t dst = x86_mem_read32(ctx, addr);
            uint32_t result = dst - src;
            x86_update_flags_sub(ctx, dst, src, result);
            x86_mem_write32(ctx, addr, result);
        }
        return 0;
    }

    // ---- SUB r32, r/m32 (0x2B) ----
    if (op == 0x2B) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        uint32_t src;
        if (modrm_mod(modrm) == 3) {
            src = *x86_reg_ptr(ctx, modrm_rm(modrm));
        } else {
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            src = x86_mem_read32(ctx, addr);
        }
        uint32_t* dst = x86_reg_ptr(ctx, modrm_reg(modrm));
        uint32_t result = *dst - src;
        x86_update_flags_sub(ctx, *dst, src, result);
        *dst = result;
        return 0;
    }

    // ---- CMP r/m32, r32 (0x39) ----
    if (op == 0x39) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        uint32_t src = *x86_reg_ptr(ctx, modrm_reg(modrm));
        uint32_t dst;
        if (modrm_mod(modrm) == 3) {
            dst = *x86_reg_ptr(ctx, modrm_rm(modrm));
        } else {
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            dst = x86_mem_read32(ctx, addr);
        }
        x86_update_flags_sub(ctx, dst, src, dst - src);
        return 0;
    }

    // ---- CMP r32, r/m32 (0x3B) ----
    if (op == 0x3B) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        uint32_t src;
        if (modrm_mod(modrm) == 3) {
            src = *x86_reg_ptr(ctx, modrm_rm(modrm));
        } else {
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            src = x86_mem_read32(ctx, addr);
        }
        uint32_t dst = *x86_reg_ptr(ctx, modrm_reg(modrm));
        x86_update_flags_sub(ctx, dst, src, dst - src);
        return 0;
    }

    // ---- AND (0x21 r/m,r ; 0x23 r,r/m ; 0x83 /4 imm8) ----
    if (op == 0x21) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        uint32_t src = *x86_reg_ptr(ctx, modrm_reg(modrm));
        if (modrm_mod(modrm) == 3) {
            uint32_t* dst = x86_reg_ptr(ctx, modrm_rm(modrm));
            *dst &= src;
            x86_update_flags_logic(ctx, *dst);
        } else {
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            uint32_t dst = x86_mem_read32(ctx, addr);
            dst &= src;
            x86_update_flags_logic(ctx, dst);
            x86_mem_write32(ctx, addr, dst);
        }
        ctx->regs.eflags &= ~X86_CF;
        ctx->regs.eflags &= ~X86_OF;
        return 0;
    }

    if (op == 0x23) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        uint32_t src;
        if (modrm_mod(modrm) == 3) {
            src = *x86_reg_ptr(ctx, modrm_rm(modrm));
        } else {
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            src = x86_mem_read32(ctx, addr);
        }
        uint32_t* dst = x86_reg_ptr(ctx, modrm_reg(modrm));
        *dst &= src;
        x86_update_flags_logic(ctx, *dst);
        ctx->regs.eflags &= ~X86_CF;
        ctx->regs.eflags &= ~X86_OF;
        return 0;
    }

    // ---- OR (0x09 r/m,r ; 0x0B r,r/m) ----
    if (op == 0x09) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        uint32_t src = *x86_reg_ptr(ctx, modrm_reg(modrm));
        if (modrm_mod(modrm) == 3) {
            uint32_t* dst = x86_reg_ptr(ctx, modrm_rm(modrm));
            *dst |= src;
            x86_update_flags_logic(ctx, *dst);
        } else {
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            uint32_t dst = x86_mem_read32(ctx, addr);
            dst |= src;
            x86_update_flags_logic(ctx, dst);
            x86_mem_write32(ctx, addr, dst);
        }
        ctx->regs.eflags &= ~X86_CF;
        ctx->regs.eflags &= ~X86_OF;
        return 0;
    }

    if (op == 0x0B) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        uint32_t src;
        if (modrm_mod(modrm) == 3) {
            src = *x86_reg_ptr(ctx, modrm_rm(modrm));
        } else {
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            src = x86_mem_read32(ctx, addr);
        }
        uint32_t* dst = x86_reg_ptr(ctx, modrm_reg(modrm));
        *dst |= src;
        x86_update_flags_logic(ctx, *dst);
        ctx->regs.eflags &= ~X86_CF;
        ctx->regs.eflags &= ~X86_OF;
        return 0;
    }

    // ---- XOR (0x31 r/m,r ; 0x33 r,r/m) ----
    if (op == 0x31) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        uint32_t src = *x86_reg_ptr(ctx, modrm_reg(modrm));
        if (modrm_mod(modrm) == 3) {
            uint32_t* dst = x86_reg_ptr(ctx, modrm_rm(modrm));
            *dst ^= src;
            x86_update_flags_logic(ctx, *dst);
        } else {
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            uint32_t dst = x86_mem_read32(ctx, addr);
            dst ^= src;
            x86_update_flags_logic(ctx, dst);
            x86_mem_write32(ctx, addr, dst);
        }
        ctx->regs.eflags &= ~X86_CF;
        ctx->regs.eflags &= ~X86_OF;
        return 0;
    }

    if (op == 0x33) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        uint32_t src;
        if (modrm_mod(modrm) == 3) {
            src = *x86_reg_ptr(ctx, modrm_rm(modrm));
        } else {
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            src = x86_mem_read32(ctx, addr);
        }
        uint32_t* dst = x86_reg_ptr(ctx, modrm_reg(modrm));
        *dst ^= src;
        x86_update_flags_logic(ctx, *dst);
        ctx->regs.eflags &= ~X86_CF;
        ctx->regs.eflags &= ~X86_OF;
        return 0;
    }

    // ---- TEST r/m32, r32 (0x85) ----
    if (op == 0x85) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        uint32_t src = *x86_reg_ptr(ctx, modrm_reg(modrm));
        uint32_t dst;
        if (modrm_mod(modrm) == 3) {
            dst = *x86_reg_ptr(ctx, modrm_rm(modrm));
        } else {
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            dst = x86_mem_read32(ctx, addr);
        }
        x86_update_flags_logic(ctx, dst & src);
        ctx->regs.eflags &= ~X86_CF;
        ctx->regs.eflags &= ~X86_OF;
        return 0;
    }

    // ---- LEA (0x8D) ----
    if (op == 0x8D) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        int consumed = 0;
        uint32_t addr = calc_ea(ctx, modrm, &consumed);
        ctx->regs.eip += consumed - 1;
        *x86_reg_ptr(ctx, modrm_reg(modrm)) = addr;
        return 0;
    }

    // ---- INC r32 (0x40-0x47) ----
    if (op >= 0x40 && op <= 0x47) {
        int reg = op - 0x40;
        uint32_t* r = x86_reg_ptr(ctx, reg);
        uint32_t cf = ctx->regs.eflags & X86_CF; // INC leaves CF untouched
        uint32_t result = *r + 1;
        x86_update_flags_add(ctx, *r, 1, result);
        ctx->regs.eflags = (ctx->regs.eflags & ~X86_CF) | cf;
        *r = result;
        return 0;
    }

    // ---- DEC r32 (0x48-0x4F) ----
    if (op >= 0x48 && op <= 0x4F) {
        int reg = op - 0x48;
        uint32_t* r = x86_reg_ptr(ctx, reg);
        uint32_t cf = ctx->regs.eflags & X86_CF; // DEC leaves CF untouched
        uint32_t result = *r - 1;
        x86_update_flags_sub(ctx, *r, 1, result);
        ctx->regs.eflags = (ctx->regs.eflags & ~X86_CF) | cf;
        *r = result;
        return 0;
    }

    // ---- IMUL r32, r/m32 (0x69 is imm, 0x6B is imm8, 0xAF is reg) ----
    if (op == 0x69) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        uint32_t src;
        if (modrm_mod(modrm) == 3) {
            src = *x86_reg_ptr(ctx, modrm_rm(modrm));
        } else {
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            src = x86_mem_read32(ctx, addr);
        }
        int32_t imm = (int32_t)read_imm32(ctx);
        uint32_t* dst = x86_reg_ptr(ctx, modrm_reg(modrm));
        *dst = src * imm;
        return 0;
    }

    if (op == 0x6B) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        uint32_t src;
        if (modrm_mod(modrm) == 3) {
            src = *x86_reg_ptr(ctx, modrm_rm(modrm));
        } else {
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
            src = x86_mem_read32(ctx, addr);
        }
        int8_t imm = (int8_t)read_imm8(ctx);
        uint32_t* dst = x86_reg_ptr(ctx, modrm_reg(modrm));
        *dst = src * imm;
        return 0;
    }

    // ---- 0x83 group (arithmetic with imm8) ----
    if (op == 0x83) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        const bool is_reg = modrm_mod(modrm) == 3;
        uint32_t addr = 0;
        if (!is_reg) {
            int consumed = 0;
            addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
        }
        int8_t imm = (int8_t)read_imm8(ctx);
        uint32_t src = (uint32_t)imm;
        int regop = modrm_reg(modrm);
        if (is_reg) {
            uint32_t* dst = x86_reg_ptr(ctx, modrm_rm(modrm));
            switch (regop) {
                case 0: { // ADD
                    uint32_t r = *dst + src;
                    x86_update_flags_add(ctx, *dst, src, r);
                    *dst = r; break;
                }
                case 1: { // OR
                    *dst |= src;
                    x86_update_flags_logic(ctx, *dst);
                    ctx->regs.eflags &= ~(X86_CF | X86_OF);
                    break;
                }
                case 2: { // ADC
                    uint32_t cf = (ctx->regs.eflags & X86_CF) ? 1 : 0;
                    uint32_t r = *dst + src + cf;
                    x86_update_flags_add(ctx, *dst, src + cf, r);
                    *dst = r; break;
                }
                case 3: { // SBB
                    uint32_t cf = (ctx->regs.eflags & X86_CF) ? 1 : 0;
                    uint32_t r = *dst - src - cf;
                    x86_update_flags_sub(ctx, *dst, src + cf, r);
                    *dst = r; break;
                }
                case 4: { // AND
                    *dst &= src;
                    x86_update_flags_logic(ctx, *dst);
                    ctx->regs.eflags &= ~(X86_CF | X86_OF);
                    break;
                }
                case 5: { // SUB
                    uint32_t r = *dst - src;
                    x86_update_flags_sub(ctx, *dst, src, r);
                    *dst = r; break;
                }
                case 6: { // XOR
                    *dst ^= src;
                    x86_update_flags_logic(ctx, *dst);
                    ctx->regs.eflags &= ~(X86_CF | X86_OF);
                    break;
                }
                case 7: { // CMP
                    x86_update_flags_sub(ctx, *dst, src, *dst - src);
                    break;
                }
            }
        } else {
            uint32_t dst = x86_mem_read32(ctx, addr);
            uint32_t result = dst;
            switch (regop) {
                case 0: result = dst + src; x86_update_flags_add(ctx, dst, src, result); break;
                case 5: result = dst - src; x86_update_flags_sub(ctx, dst, src, result); break;
                case 7: x86_update_flags_sub(ctx, dst, src, dst - src); result = dst; break;
                case 4: result = dst & src; x86_update_flags_logic(ctx, result); ctx->regs.eflags &= ~(X86_CF | X86_OF); break;
                case 1: result = dst | src; x86_update_flags_logic(ctx, result); ctx->regs.eflags &= ~(X86_CF | X86_OF); break;
                case 6: result = dst ^ src; x86_update_flags_logic(ctx, result); ctx->regs.eflags &= ~(X86_CF | X86_OF); break;
            }
            if (regop != 7) x86_mem_write32(ctx, addr, result);
        }
        return 0;
    }

    // ---- 0x81 group (arithmetic with imm32) ----
    if (op == 0x81) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        const bool is_reg = modrm_mod(modrm) == 3;
        uint32_t addr = 0;
        if (!is_reg) {
            int consumed = 0;
            addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
        }
        uint32_t imm = read_imm32(ctx);
        int regop = modrm_reg(modrm);
        if (is_reg) {
            uint32_t* dst = x86_reg_ptr(ctx, modrm_rm(modrm));
            switch (regop) {
                case 0: { uint32_t r = *dst + imm; x86_update_flags_add(ctx, *dst, imm, r); *dst = r; break; }
                case 5: { uint32_t r = *dst - imm; x86_update_flags_sub(ctx, *dst, imm, r); *dst = r; break; }
                case 7: { x86_update_flags_sub(ctx, *dst, imm, *dst - imm); break; }
                case 4: { *dst &= imm; x86_update_flags_logic(ctx, *dst); ctx->regs.eflags &= ~(X86_CF | X86_OF); break; }
                case 1: { *dst |= imm; x86_update_flags_logic(ctx, *dst); ctx->regs.eflags &= ~(X86_CF | X86_OF); break; }
                case 6: { *dst ^= imm; x86_update_flags_logic(ctx, *dst); ctx->regs.eflags &= ~(X86_CF | X86_OF); break; }
            }
        } else {
            uint32_t dst = x86_mem_read32(ctx, addr);
            uint32_t result = dst;
            switch (regop) {
                case 0: result = dst + imm; x86_update_flags_add(ctx, dst, imm, result); break;
                case 5: result = dst - imm; x86_update_flags_sub(ctx, dst, imm, result); break;
                case 7: x86_update_flags_sub(ctx, dst, imm, dst - imm); result = dst; break;
                case 4: result = dst & imm; x86_update_flags_logic(ctx, result); ctx->regs.eflags &= ~(X86_CF | X86_OF); break;
                case 1: result = dst | imm; x86_update_flags_logic(ctx, result); ctx->regs.eflags &= ~(X86_CF | X86_OF); break;
                case 6: result = dst ^ imm; x86_update_flags_logic(ctx, result); ctx->regs.eflags &= ~(X86_CF | X86_OF); break;
            }
            if (regop != 7) x86_mem_write32(ctx, addr, result);
        }
        return 0;
    }

    // ---- ALU EAX, imm32 (0x05/0x0D/0x15/0x1D/0x25/0x2D/0x35/0x3D) ----
    // ---- ALU AL, imm8  (0x04/0x0C/0x14/0x1C/0x24/0x2C/0x34/0x3C) ----
    // The short accumulator forms: same /r extension numbering as 0x81, no
    // ModRM. OpenTTD's CRT hits 0x35 (`xor eax, imm32`) checking the CPU id.
    if (((op & 7) == 5 || (op & 7) == 4) && op <= 0x3D) {
        const int regop = op >> 3;      // 0 ADD 1 OR 2 ADC 3 SBB 4 AND 5 SUB 6 XOR 7 CMP
        const int bits = (op & 7) == 5 ? 32 : 8;
        const uint32_t imm = (op & 7) == 5 ? read_imm32(ctx) : (uint32_t)read_imm8(ctx);
        const uint32_t dst = bits == 32 ? ctx->regs.eax : (ctx->regs.eax & 0xFF);
        uint32_t result = dst;
        const uint32_t cf = (ctx->regs.eflags & X86_CF) ? 1u : 0u;
        switch (regop) {
            case 0: result = dst + imm; x86_update_flags_add(ctx, dst, imm, result); break;
            case 1: result = dst | imm; x86_update_flags_logic(ctx, result); ctx->regs.eflags &= ~(X86_CF | X86_OF); break;
            case 2: result = dst + imm + cf; x86_update_flags_add(ctx, dst, imm + cf, result); break;
            case 3: result = dst - imm - cf; x86_update_flags_sub(ctx, dst, imm + cf, result); break;
            case 4: result = dst & imm; x86_update_flags_logic(ctx, result); ctx->regs.eflags &= ~(X86_CF | X86_OF); break;
            case 5: result = dst - imm; x86_update_flags_sub(ctx, dst, imm, result); break;
            case 6: result = dst ^ imm; x86_update_flags_logic(ctx, result); ctx->regs.eflags &= ~(X86_CF | X86_OF); break;
            case 7: x86_update_flags_sub(ctx, dst, imm, dst - imm); result = dst; break;
        }
        if (regop != 7) {
            if (bits == 32) ctx->regs.eax = result;
            else ctx->regs.eax = (ctx->regs.eax & 0xFFFFFF00) | (result & 0xFF);
        }
        return 0;
    }

    // Group 2 word/dword shifts and rotates, with immediate, one or CL.
    if (op == 0xC1 || op == 0xD1 || op == 0xD3) {
        const uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        const int operation = modrm_reg(modrm);
        const bool narrow = operand_size == 16;
        const unsigned width = narrow ? 16 : 32;
        const uint64_t mask = (uint64_t{1} << width) - 1;
        const uint64_t sign = uint64_t{1} << (width - 1);
        const bool is_reg = modrm_mod(modrm) == 3;
        uint32_t address = 0;
        if (!is_reg) {
            int consumed = 0;
            address = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
        }
        const unsigned count = (op == 0xC1 ? read_imm8(ctx)
                                : op == 0xD1 ? 1 : ctx->regs.ecx) & 31u;
        if (count == 0) return 0;

        uint32_t* reg = is_reg ? x86_reg_ptr(ctx, modrm_rm(modrm)) : nullptr;
        const uint64_t value = (is_reg ? *reg : narrow ? x86_mem_read16(ctx, address)
                                                      : x86_mem_read32(ctx, address)) & mask;
        uint64_t result = value;
        bool carry = false, overflow = false;
        if (operation < 4) {
            const bool through_carry = operation >= 2;
            const bool right = (operation & 1) != 0;
            const unsigned ring_width = width + unsigned(through_carry);
            const unsigned rotate = count % ring_width;
            if (through_carry && rotate == 0) return 0;
            uint64_t ring = value;
            if (through_carry && (ctx->regs.eflags & X86_CF)) ring |= uint64_t{1} << width;
            if (rotate) {
                const auto complement = ring_width - rotate;
                ring = right ? (ring >> rotate) | (ring << complement)
                             : (ring << rotate) | (ring >> complement);
            }
            result = ring & mask;
            carry = through_carry ? ((ring >> width) & 1) != 0
                                  : right ? (result & sign) != 0 : (result & 1) != 0;
            overflow = right ? ((result ^ (result << 1)) & sign) != 0
                             : ((result & sign) != 0) != carry;
        } else {
            const bool negative = (value & sign) != 0;
            if (operation == 4 || operation == 6) {
                result = (value << count) & mask;
                carry = count <= width && ((value >> (width - count)) & 1) != 0;
                overflow = ((result & sign) != 0) != carry;
            } else {
                carry = count <= width ? ((value >> (count - 1)) & 1) != 0
                                       : operation == 7 && negative;
                result = value >> count;
                if (operation == 7 && negative)
                    result |= count >= width ? mask : mask ^ (mask >> count);
                overflow = operation == 5 && negative;
            }
            ctx->regs.eflags &= ~(X86_ZF | X86_SF | X86_PF);
            if (result == 0) ctx->regs.eflags |= X86_ZF;
            if (result & sign) ctx->regs.eflags |= X86_SF;
            if ((__builtin_popcount(unsigned(result & 0xFF)) & 1) == 0)
                ctx->regs.eflags |= X86_PF;
        }
        ctx->regs.eflags = (ctx->regs.eflags & ~X86_CF) | (carry ? X86_CF : 0);
        if (count == 1)
            ctx->regs.eflags = (ctx->regs.eflags & ~X86_OF) | (overflow ? X86_OF : 0);
        if (reg) *reg = narrow ? (*reg & 0xFFFF0000u) | uint32_t(result) : uint32_t(result);
        else if (narrow) x86_mem_write16(ctx, address, uint16_t(result));
        else x86_mem_write32(ctx, address, uint32_t(result));
        return 0;
    }

    // ---- XCHG (0x87 r/m,r ; 0x90+reg is NOP/XCHG EAX) ----
    // The memory form is how UCRT publishes a runtime-resolved function
    // pointer (the atomic `xchg [slot], eax` idiom). It MUST swap through the
    // effective address and consume the ModRM/SIB/displacement bytes; a
    // register-only implementation left the store unwritten and the address
    // bytes to be decoded as instructions.
    if (op == 0x87) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        uint32_t* r = x86_reg_ptr(ctx, modrm_reg(modrm));
        if (modrm_mod(modrm) == 3) {
            uint32_t* p = x86_reg_ptr(ctx, modrm_rm(modrm));
            uint32_t t = *p; *p = *r; *r = t;
        } else {
            int c = 0;
            const uint32_t ad = calc_ea(ctx, modrm, &c);
            ctx->regs.eip += c - 1;
            const uint32_t t = x86_mem_read32(ctx, ad);
            x86_mem_write32(ctx, ad, *r);
            *r = t;
        }
        return 0;
    }

    // ---- XCHG EAX, r32 (0x91-0x97) ----
    if (op >= 0x91 && op <= 0x97) {
        int reg = op - 0x90;
        uint32_t t = ctx->regs.eax;
        ctx->regs.eax = *x86_reg_ptr(ctx, reg);
        *x86_reg_ptr(ctx, reg) = t;
        return 0;
    }

    // ---- CDQ (0x99) ----
    if (op == 0x99) {
        ctx->regs.edx = (ctx->regs.eax & 0x80000000) ? 0xFFFFFFFF : 0;
        return 0;
    }

    // ---- CWD/CDQ variants ----
    if (op == 0x98) { // CWDE
        ctx->regs.eax = (int32_t)(int16_t)(ctx->regs.eax & 0xFFFF);
        return 0;
    }

    // ---- LEAVE (0xC9) ----
    if (op == 0xC9) {
        ctx->regs.esp = ctx->regs.ebp;
        ctx->regs.ebp = x86_mem_read32(ctx, ctx->regs.esp);
        ctx->regs.esp += 4;
        return 0;
    }

    // ---- ENTER (0xC8) ----
    if (op == 0xC8) {
        uint16_t size = x86_mem_read16(ctx, ctx->regs.eip);
        ctx->regs.eip += 2;
        uint8_t level = read_imm8(ctx);
        ctx->regs.esp -= 4;
        x86_mem_write32(ctx, ctx->regs.esp, ctx->regs.ebp);
        ctx->regs.ebp = ctx->regs.esp;
        ctx->regs.esp -= size;
        (void)level;
        return 0;
    }

    // ---- 0xF7 group: TEST/NOT/NEG/MUL/IMUL/DIV/IDIV r/m32 ----
    // Every extension, register and memory form. The old version only handled
    // the register forms of /2../7, so `test ebx, 0x200` (OpenTTD's CPU probe,
    // F7 C3 00 02 00 00) fell off the end as UNHANDLED.
    if (op == 0xF7) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        const int regop = modrm_reg(modrm);
        const bool is_reg = (modrm_mod(modrm) == 3);
        uint32_t addr = 0;
        if (!is_reg) {
            int consumed = 0;
            addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
        }
        const uint32_t v = is_reg ? *x86_reg_ptr(ctx, modrm_rm(modrm))
                                  : x86_mem_read32(ctx, addr);
        switch (regop) {
            case 0: case 1: { // TEST r/m32, imm32 (flags only)
                const uint32_t imm = read_imm32(ctx);
                x86_update_flags_logic(ctx, v & imm);
                ctx->regs.eflags &= ~(X86_CF | X86_OF);
                break;
            }
            case 2: { // NOT (flags unaffected)
                const uint32_t r = ~v;
                if (is_reg) *x86_reg_ptr(ctx, modrm_rm(modrm)) = r;
                else x86_mem_write32(ctx, addr, r);
                break;
            }
            case 3: { // NEG
                const uint32_t r = 0u - v;
                x86_update_flags_sub(ctx, 0, v, r);
                ctx->regs.eflags = (ctx->regs.eflags & ~X86_CF) | (v ? X86_CF : 0);
                if (is_reg) *x86_reg_ptr(ctx, modrm_rm(modrm)) = r;
                else x86_mem_write32(ctx, addr, r);
                break;
            }
            case 4: { // MUL: EDX:EAX = EAX * v
                const uint64_t r = (uint64_t)ctx->regs.eax * v;
                ctx->regs.eax = (uint32_t)r;
                ctx->regs.edx = (uint32_t)(r >> 32);
                ctx->regs.eflags &= ~(X86_CF | X86_OF);
                if (ctx->regs.edx) ctx->regs.eflags |= X86_CF | X86_OF;
                break;
            }
            case 5: { // IMUL (signed, EDX:EAX)
                const int64_t r = (int64_t)(int32_t)ctx->regs.eax * (int64_t)(int32_t)v;
                ctx->regs.eax = (uint32_t)r;
                ctx->regs.edx = (uint32_t)(r >> 32);
                const bool overflow = ((int64_t)(int32_t)ctx->regs.eax != r);
                ctx->regs.eflags &= ~(X86_CF | X86_OF);
                if (overflow) ctx->regs.eflags |= X86_CF | X86_OF;
                break;
            }
            case 6: { // DIV (unsigned)
                if (v == 0) {
                    if (ctx->log) ctx->log("x86: DIV by zero");
                    ctx->running = 0;
                    return -1;
                }
                const uint64_t dividend = ((uint64_t)ctx->regs.edx << 32) | ctx->regs.eax;
                ctx->regs.eax = (uint32_t)(dividend / v);
                ctx->regs.edx = (uint32_t)(dividend % v);
                break;
            }
            case 7: { // IDIV (signed)
                if (v == 0) {
                    if (ctx->log) ctx->log("x86: IDIV by zero");
                    ctx->running = 0;
                    return -1;
                }
                const int64_t dividend = ((int64_t)(int32_t)ctx->regs.edx << 32) |
                                         (uint32_t)ctx->regs.eax;
                const int32_t divisor = (int32_t)v;
                ctx->regs.eax = (uint32_t)(dividend / divisor);
                ctx->regs.edx = (uint32_t)(dividend % divisor);
                break;
            }
        }
        return 0;
    }

    // INC/DEC r/m8. Consume any address bytes before the next instruction.
    if (op == 0xFE) {
        const uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        const int operation = modrm_reg(modrm);
        if (operation > 1) {
            if (ctx->log) ctx->log("x86: unsupported FE extension");
            ctx->running = 0;
            return -1;
        }
        const bool is_reg = modrm_mod(modrm) == 3;
        uint32_t addr = 0;
        if (!is_reg) {
            int consumed = 0;
            addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;
        }
        const int rm = modrm_rm(modrm);
        const uint8_t original = is_reg ? *r8p(ctx, rm) : x86_mem_read8(ctx, addr);
        const auto carry = ctx->regs.eflags & X86_CF;
        const unsigned result = operation == 0 ? unsigned(original) + 1 : unsigned(original) - 1;
        if (operation == 0) flag_add8(ctx, original, 1, 0, result);
        else flag_sub8(ctx, original, 1, 0, result);
        ctx->regs.eflags = (ctx->regs.eflags & ~X86_CF) | carry;
        if (is_reg) *r8p(ctx, rm) = uint8_t(result);
        else x86_mem_write8(ctx, addr, uint8_t(result));
        return 0;
    }

    // ---- String instructions (MOVS/STOS/LODS/SCAS/CMPS) with REP prefixes ----
    if (op == 0xA4 || op == 0xA5) { // MOVSB / MOVSD
        uint32_t step = (op == 0xA4) ? 1u : 4u;
        uint32_t n = rep ? ctx->regs.ecx : 1u;
        int df = (ctx->regs.eflags & X86_DF) != 0;
        while (n) {
            if (step == 1) x86_mem_write8(ctx, ctx->regs.edi, x86_mem_read8(ctx, ctx->regs.esi));
            else x86_mem_write32(ctx, ctx->regs.edi, x86_mem_read32(ctx, ctx->regs.esi));
            if (df) { ctx->regs.esi -= step; ctx->regs.edi -= step; }
            else { ctx->regs.esi += step; ctx->regs.edi += step; }
            if (!rep) break;
            n--; ctx->regs.ecx = n;
        }
        return 0;
    }
    if (op == 0xAA || op == 0xAB) { // STOSB / STOSD
        uint32_t step = (op == 0xAA) ? 1u : 4u;
        uint32_t n = rep ? ctx->regs.ecx : 1u;
        int df = (ctx->regs.eflags & X86_DF) != 0;
        while (n) {
            if (step == 1) x86_mem_write8(ctx, ctx->regs.edi, (uint8_t)ctx->regs.eax);
            else x86_mem_write32(ctx, ctx->regs.edi, ctx->regs.eax);
            if (df) ctx->regs.edi -= step; else ctx->regs.edi += step;
            if (!rep) break;
            n--; ctx->regs.ecx = n;
        }
        return 0;
    }
    if (op == 0xAC || op == 0xAD) { // LODSB / LODSD
        uint32_t step = (op == 0xAC) ? 1u : 4u;
        uint32_t n = rep ? ctx->regs.ecx : 1u;
        int df = (ctx->regs.eflags & X86_DF) != 0;
        while (n) {
            if (step == 1) ctx->regs.eax = (ctx->regs.eax & 0xFFFFFF00u) | x86_mem_read8(ctx, ctx->regs.esi);
            else ctx->regs.eax = x86_mem_read32(ctx, ctx->regs.esi);
            if (df) ctx->regs.esi -= step; else ctx->regs.esi += step;
            if (!rep) break;
            n--; ctx->regs.ecx = n;
        }
        return 0;
    }
    if (op == 0xAE || op == 0xAF) { // SCASB / SCASD
        uint32_t step = (op == 0xAE) ? 1u : 4u;
        uint32_t n = rep ? ctx->regs.ecx : 1u;
        int df = (ctx->regs.eflags & X86_DF) != 0;
        while (n) {
            if (step == 1) {
                uint32_t val = x86_mem_read8(ctx, ctx->regs.edi);
                uint32_t acc = ctx->regs.eax & 0xFF;
                x86_update_flags_sub(ctx, acc, val, acc - val);
            } else {
                uint32_t val = x86_mem_read32(ctx, ctx->regs.edi);
                x86_update_flags_sub(ctx, ctx->regs.eax, val, ctx->regs.eax - val);
            }
            if (df) ctx->regs.edi -= step; else ctx->regs.edi += step;
            if (!rep) break;
            n--; ctx->regs.ecx = n;
            if (n == 0) break;
            int zf = (ctx->regs.eflags & X86_ZF) != 0;
            if (rep == 1 && !zf) break; // REPE
            if (rep == 2 && zf) break;  // REPNE
        }
        return 0;
    }
    if (op == 0xA6 || op == 0xA7) { // CMPSB / CMPSD
        uint32_t step = (op == 0xA6) ? 1u : 4u;
        uint32_t n = rep ? ctx->regs.ecx : 1u;
        int df = (ctx->regs.eflags & X86_DF) != 0;
        while (n) {
            uint32_t a, b;
            if (step == 1) { a = x86_mem_read8(ctx, ctx->regs.esi); b = x86_mem_read8(ctx, ctx->regs.edi); }
            else { a = x86_mem_read32(ctx, ctx->regs.esi); b = x86_mem_read32(ctx, ctx->regs.edi); }
            x86_update_flags_sub(ctx, a, b, a - b);
            if (df) { ctx->regs.esi -= step; ctx->regs.edi -= step; }
            else { ctx->regs.esi += step; ctx->regs.edi += step; }
            if (!rep) break;
            n--; ctx->regs.ecx = n;
            if (n == 0) break;
            int zf = (ctx->regs.eflags & X86_ZF) != 0;
            if (rep == 1 && !zf) break;
            if (rep == 2 && zf) break;
        }
        return 0;
    }

    // ---- IN/OUT (0xE4-E7, 0xEC-EF) - stubbed ----
    if (op >= 0xE4 && op <= 0xE7) {
        read_imm8(ctx);
        if (op <= 0xE5) read_imm8(ctx); // 16-bit port
        return 0;
    }
    if (op >= 0xEC && op <= 0xEF) {
        return 0;
    }

    // ---- FPU instructions (D8-DF) ----
    if (op >= 0xD8 && op <= 0xDF) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        int mod = modrm_mod(modrm);
        int regop = modrm_reg(modrm);
        int rm = modrm_rm(modrm);

        if (mod == 3) {
            // Register-register operations
            if (op == 0xD8) {
                // D8 /0 = FADD, /1 = FMUL, /2 = FCOM, /3 = FCOMP
                // /4 = FSUB, /5 = FSUBR, /6 = FDIV, /7 = FDIVR
                double st0 = fpu_st(ctx, 0);
                (void)regop; // suppress unused
                switch (rm) { // actually regop is the operation for D8
                }
                // D8 with mod=3: operation determined by regop field of modrm
                // Wait, for D8 mod=3, the 3-bit reg field is the operation:
                // 000=FADD, 001=FMUL, 010=FCOM, 011=FCOMP
                // 100=FSUB, 101=FSUBR, 110=FDIV, 111=FDIVR
                // and rm selects ST(i)
                double val = fpu_st(ctx, rm);
                switch (regop) {
                    case 0: fpu_set_st(ctx, 0, st0 + val); break; // FADD
                    case 1: fpu_set_st(ctx, 0, st0 * val); break; // FMUL
                    case 2: { // FCOM
                        // Compare ST0 with ST(i), set FPU status
                        ctx->regs.fpu_sw &= ~0x4500;
                        if (st0 > val) { /* greater */ }
                        else if (st0 < val) ctx->regs.fpu_sw |= 0x0100; // less
                        else ctx->regs.fpu_sw |= 0x4000; // equal
                        break;
                    }
                    case 3: { // FCOMP
                        ctx->regs.fpu_sw &= ~0x4500;
                        if (st0 < val) ctx->regs.fpu_sw |= 0x0100;
                        else if (st0 == val) ctx->regs.fpu_sw |= 0x4000;
                        fpu_pop(ctx);
                        break;
                    }
                    case 4: fpu_set_st(ctx, 0, st0 - val); break; // FSUB
                    case 5: fpu_set_st(ctx, 0, val - st0); break; // FSUBR
                    case 6: fpu_set_st(ctx, 0, st0 / val); break; // FDIV
                    case 7: fpu_set_st(ctx, 0, val / st0); break; // FDIVR
                }
            } else if (op == 0xD9) {
                // D9 mod=3: 
                // 000=FLD, 001=FXCH, 010=FNOP, 011=FSTP (some)
                // 100=FCHS/FABS/FTST/FXAM, 101=FLD1/FLDZ/etc, 110=F2XM1/etc, 111=...
                switch (regop) {
                    case 0: // FLD ST(i)
                        fpu_push(ctx, fpu_st(ctx, rm + 1)); // +1 because after push
                        break;
                    case 1: { // FXCH ST(i)
                        double t = fpu_st(ctx, 0);
                        fpu_set_st(ctx, 0, fpu_st(ctx, rm));
                        fpu_set_st(ctx, rm, t);
                        break;
                    }
                    case 2: break; // FNOP
                    case 4: { // FCHS/FABS/FTST/FXAM
                        switch (rm) {
                            case 0: fpu_set_st(ctx, 0, -fpu_st(ctx, 0)); break; // FCHS
                            case 1: fpu_set_st(ctx, 0, (fpu_st(ctx, 0) < 0) ? -fpu_st(ctx, 0) : fpu_st(ctx, 0)); break; // FABS
                            case 2: { // FTST
                                double v = fpu_st(ctx, 0);
                                ctx->regs.fpu_sw &= ~0x4500;
                                if (v < 0) ctx->regs.fpu_sw |= 0x0100;
                                else if (v == 0) ctx->regs.fpu_sw |= 0x4000;
                                break;
                            }
                            case 4: break; // FLDENV (skip)
                        }
                        break;
                    }
                    case 5: { // FLD1, FLDZ, etc.
                        switch (rm) {
                            case 0: fpu_push(ctx, 1.0); break; // FLD1
                            case 2: fpu_push(ctx, 0.0); break; // FLDZ
                            case 5: fpu_push(ctx, 3.14159265358979323846); break; // FLDPI
                            case 1: fpu_push(ctx, 0.301029995663981195); break; // FLDL2T
                            case 3: fpu_push(ctx, 0.6931471805599453); break; // FLDLN2
                            case 4: fpu_push(ctx, 3.321928094887362); break; // FLDL2E
                            case 6: fpu_push(ctx, 1.4426950408889634); break; // FLDLG2
                        }
                        break;
                    }
                    case 6: { // F2XM1, FYL2X, etc. (approximate)
                        switch (rm) {
                            case 0: fpu_set_st(ctx, 0, pow(2.0, fpu_st(ctx, 0)) - 1.0); break; // F2XM1
                            case 4: fpu_set_st(ctx, 0, sqrt(fpu_st(ctx, 0))); break; // FSQRT
                        }
                        break;
                    }
                    case 7: { // FPREM, FRNDINT, etc.
                        switch (rm) {
                            case 0: fpu_set_st(ctx, 0, (double)(int)fpu_st(ctx, 0)); break; // FRNDINT (truncate)
                            case 4: fpu_set_st(ctx, 0, sqrt(fpu_st(ctx, 0))); break; // FSQRT (alt)
                        }
                        break;
                    }
                }
            } else if (op == 0xDB) {
                switch (regop) {
                    case 0: // FILD (int32 -> float, memory handled below, this is reg)
                        // DB /0 with mod=3 is FCMOVNB (P6+), skip
                        break;
                    case 5: { // FUCOMI
                        double st0 = fpu_st(ctx, 0);
                        double sti = fpu_st(ctx, rm);
                        ctx->regs.eflags &= ~(X86_ZF | X86_PF | X86_CF);
                        if (st0 < sti) ctx->regs.eflags |= X86_CF;
                        if (st0 == sti) ctx->regs.eflags |= X86_ZF;
                        // parity for unordered
                        break;
                    }
                    case 6: { // FCOMI
                        double st0 = fpu_st(ctx, 0);
                        double sti = fpu_st(ctx, rm);
                        ctx->regs.eflags &= ~(X86_ZF | X86_PF | X86_CF);
                        if (st0 < sti) ctx->regs.eflags |= X86_CF;
                        if (st0 == sti) ctx->regs.eflags |= X86_ZF;
                        break;
                    }
                }
            } else if (op == 0xDD) {
                switch (regop) {
                    case 0: // FLD (double, memory) or FFREE (reg)
                        if (rm == 0) {
                            // FFREE ST(0)
                            ctx->regs.fpu_tags[ctx->regs.fpu_top] = 3;
                        }
                        break;
                    case 2: // FST (double, memory) or FST (reg) - no pop
                        break;
                    case 3: // FSTP (double, memory/reg) - pop
                        if (rm >= 0) {
                            // FSTP ST(i)
                            fpu_set_st(ctx, rm, fpu_st(ctx, 0));
                            fpu_pop(ctx);
                        }
                        break;
                }
            } else if (op == 0xDE) {
                switch (regop) {
                    case 0: // FADDP
                        fpu_set_st(ctx, rm + 1, fpu_st(ctx, rm + 1) + fpu_st(ctx, 0));
                        fpu_pop(ctx);
                        break;
                    case 1: // FMULP
                        fpu_set_st(ctx, rm + 1, fpu_st(ctx, rm + 1) * fpu_st(ctx, 0));
                        fpu_pop(ctx);
                        break;
                    case 3: // FCOMPP
                        {
                            double a = fpu_st(ctx, 0);
                            double b = fpu_st(ctx, 1);
                            ctx->regs.fpu_sw &= ~0x4500;
                            if (a < b) ctx->regs.fpu_sw |= 0x0100;
                            else if (a == b) ctx->regs.fpu_sw |= 0x4000;
                            fpu_pop(ctx);
                            fpu_pop(ctx);
                        }
                        break;
                    case 4: // FSUBP
                        fpu_set_st(ctx, rm + 1, fpu_st(ctx, rm + 1) - fpu_st(ctx, 0));
                        fpu_pop(ctx);
                        break;
                    case 5: // FSUBRP
                        fpu_set_st(ctx, rm + 1, fpu_st(ctx, 0) - fpu_st(ctx, rm + 1));
                        fpu_pop(ctx);
                        break;
                    case 6: // FDIVP
                        fpu_set_st(ctx, rm + 1, fpu_st(ctx, rm + 1) / fpu_st(ctx, 0));
                        fpu_pop(ctx);
                        break;
                    case 7: // FDIVRP
                        fpu_set_st(ctx, rm + 1, fpu_st(ctx, 0) / fpu_st(ctx, rm + 1));
                        fpu_pop(ctx);
                        break;
                }
            } else if (op == 0xDF) {
                switch (regop) {
                    case 4: // FNSTSW AX
                        ctx->regs.eax = (ctx->regs.eax & 0xFFFF0000) | ctx->regs.fpu_sw;
                        break;
                }
            }
        } else {
            // Memory operations
            int consumed = 0;
            uint32_t addr = calc_ea(ctx, modrm, &consumed);
            ctx->regs.eip += consumed - 1;

            if (op == 0xD8) {
                // D8: float (32-bit) memory
                float val;
                uint32_t _tmp = x86_mem_read32(ctx, addr);
                memcpy(&val, &_tmp, 4);
                double dval = val;
                switch (regop) {
                    case 0: fpu_push(ctx, dval); break; // FLD (float)
                    case 1: fpu_set_st(ctx, 0, fpu_st(ctx, 0) * dval); break; // FMUL
                    case 2: { // FCOM
                        double st0 = fpu_st(ctx, 0);
                        ctx->regs.fpu_sw &= ~0x4500;
                        if (st0 < dval) ctx->regs.fpu_sw |= 0x0100;
                        else if (st0 == dval) ctx->regs.fpu_sw |= 0x4000;
                        break;
                    }
                    case 3: { // FCOMP
                        double st0 = fpu_st(ctx, 0);
                        ctx->regs.fpu_sw &= ~0x4500;
                        if (st0 < dval) ctx->regs.fpu_sw |= 0x0100;
                        else if (st0 == dval) ctx->regs.fpu_sw |= 0x4000;
                        fpu_pop(ctx);
                        break;
                    }
                    case 4: fpu_set_st(ctx, 0, fpu_st(ctx, 0) - dval); break; // FSUB
                    case 5: fpu_set_st(ctx, 0, dval - fpu_st(ctx, 0)); break; // FSUBR
                    case 6: fpu_set_st(ctx, 0, fpu_st(ctx, 0) / dval); break; // FDIV
                    case 7: fpu_set_st(ctx, 0, dval / fpu_st(ctx, 0)); break; // FDIVR
                }
            } else if (op == 0xD9) {
                switch (regop) {
                    case 0: { // FLD (float memory)
                        float val; uint32_t _tmp2 = x86_mem_read32(ctx, addr); memcpy(&val, &_tmp2, 4);
                        fpu_push(ctx, val);
                        break;
                    }
                    case 2: { // FST (float memory)
                        float val = (float)fpu_st(ctx, 0);
                        uint32_t raw; memcpy(&raw, &val, 4);
                        x86_mem_write32(ctx, addr, raw);
                        break;
                    }
                    case 3: { // FSTP (float memory)
                        float val = (float)fpu_st(ctx, 0);
                        uint32_t raw; memcpy(&raw, &val, 4);
                        x86_mem_write32(ctx, addr, raw);
                        fpu_pop(ctx);
                        break;
                    }
                    case 4: { // FLDENV
                        fpu_read_env(ctx, addr);
                        break;
                    }
                    case 5: { // FLDCW
                        ctx->regs.fpu_cw = x86_mem_read16(ctx, addr);
                        break;
                    }
                    case 6: { // FNSTENV
                        fpu_write_env(ctx, addr);
                        break;
                    }
                    case 7: { // FNSTCW
                        x86_mem_write16(ctx, addr, ctx->regs.fpu_cw);
                        break;
                    }
                }
            } else if (op == 0xDB) {
                switch (regop) {
                    case 0: { // FILD (int32 memory)
                        int32_t val = (int32_t)x86_mem_read32(ctx, addr);
                        fpu_push(ctx, (double)val);
                        break;
                    }
                    case 2: { // FIST (int32 memory)
                        int32_t val = (int32_t)fpu_to_int(ctx, fpu_st(ctx, 0));
                        x86_mem_write32(ctx, addr, (uint32_t)val);
                        break;
                    }
                    case 3: { // FISTP (int32 memory)
                        int32_t val = (int32_t)fpu_to_int(ctx, fpu_st(ctx, 0));
                        x86_mem_write32(ctx, addr, (uint32_t)val);
                        fpu_pop(ctx);
                        break;
                    }
                    case 5: { // FLDCW (word)
                        ctx->regs.fpu_cw = x86_mem_read16(ctx, addr);
                        break;
                    }
                }
            } else if (op == 0xDD) {
                switch (regop) {
                    case 0: { // FLD (double memory)
                        uint32_t lo = x86_mem_read32(ctx, addr);
                        uint32_t hi = x86_mem_read32(ctx, addr + 4);
                        uint64_t raw = ((uint64_t)hi << 32) | lo;
                        double val; memcpy(&val, &raw, 8);
                        fpu_push(ctx, val);
                        break;
                    }
                    case 2: { // FST (double memory)
                        double _d = fpu_st(ctx, 0); uint64_t raw; memcpy(&raw, &_d, 8);
                        x86_mem_write32(ctx, addr, (uint32_t)(raw & 0xFFFFFFFF));
                        x86_mem_write32(ctx, addr + 4, (uint32_t)(raw >> 32));
                        break;
                    }
                    case 3: { // FSTP (double memory)
                        double _d = fpu_st(ctx, 0); uint64_t raw; memcpy(&raw, &_d, 8);
                        x86_mem_write32(ctx, addr, (uint32_t)(raw & 0xFFFFFFFF));
                        x86_mem_write32(ctx, addr + 4, (uint32_t)(raw >> 32));
                        fpu_pop(ctx);
                        break;
                    }
                    case 4: { // FRSTOR
                        fpu_read_save(ctx, addr);
                        break;
                    }
                    case 6: { // FNSAVE
                        fpu_write_save(ctx, addr);
                        break;
                    }
                    case 7: { // FNSTSW (memory, word)
                        x86_mem_write16(ctx, addr, ctx->regs.fpu_sw);
                        break;
                    }
                }
            } else if (op == 0xDE) {
                switch (regop) {
                    case 0: { // FIADD (int32 memory)
                        int32_t val = (int32_t)x86_mem_read32(ctx, addr);
                        fpu_set_st(ctx, 0, fpu_st(ctx, 0) + val);
                        break;
                    }
                    case 3: { // FICOMP (int32 memory)
                        int32_t val = (int32_t)x86_mem_read32(ctx, addr);
                        double dval = val;
                        ctx->regs.fpu_sw &= ~0x4500;
                        double st0 = fpu_st(ctx, 0);
                        if (st0 < dval) ctx->regs.fpu_sw |= 0x0100;
                        else if (st0 == dval) ctx->regs.fpu_sw |= 0x4000;
                        fpu_pop(ctx);
                        break;
                    }
                    case 4: { // FISUB (int32 memory)
                        int32_t val = (int32_t)x86_mem_read32(ctx, addr);
                        fpu_set_st(ctx, 0, fpu_st(ctx, 0) - val);
                        break;
                    }
                    case 6: { // FIDIV (int32 memory)
                        int32_t val = (int32_t)x86_mem_read32(ctx, addr);
                        fpu_set_st(ctx, 0, fpu_st(ctx, 0) / val);
                        break;
                    }
                }
            }
        }
        return 0;
    }

    // ---- 8-bit / immediate-form arithmetic (0x00-0x3D, 0x80, 0x84, 0xA8, 0xA9) ----
    if (op <= 0x3D && (op & 7) <= 4 && ((op & 0x07) == 0 || (op & 0x07) == 2 || (op & 0x07) == 4 || (op & 0x07) == 1 || (op & 0x07) == 3)) {
        int kind = (op >> 3) & 7; // 0 add 1 or 2 adc 3 sbb 4 and 5 sub 6 xor 7 cmp
        int form = op & 7;        // 0 r/m8,r8 | 1 r/m32,r32 | 2 r8,r/m8 | 3 r32,r/m32 | 4 AL,imm8
        if (form == 0 || form == 2) {
            uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
            int is_reg = (modrm_mod(modrm) == 3);
            uint8_t* dptr = nullptr;
            uint32_t daddr = 0;
            if (is_reg) dptr = r8p(ctx, modrm_rm(modrm));
            else { int c = 0; daddr = calc_ea(ctx, modrm, &c); ctx->regs.eip += c - 1; }
            uint8_t srcv = is_reg ? *r8p(ctx, modrm_reg(modrm)) : 0;
            uint8_t dstv = is_reg ? *dptr : x86_mem_read8(ctx, daddr);
            if (form == 2) { // r8 <- r/m8: operands swap
                uint8_t t = dstv; dstv = srcv; srcv = t;
                // destination is the reg field
                dptr = r8p(ctx, modrm_reg(modrm));
                is_reg = 1;
            }
            unsigned r;
            switch (kind) {
                case 0: r = (unsigned)dstv + srcv; flag_add8(ctx, dstv, srcv, 0, r); break;
                case 1: r = dstv | srcv; flag_logic8(ctx, (uint8_t)r); break;
                case 2: r = (unsigned)dstv + srcv + ((ctx->regs.eflags & X86_CF) ? 1 : 0); flag_add8(ctx, dstv, srcv, 0, r); break;
                case 3: r = (unsigned)dstv - srcv - ((ctx->regs.eflags & X86_CF) ? 1 : 0); flag_sub8(ctx, dstv, srcv, 0, r); break;
                case 4: r = dstv & srcv; flag_logic8(ctx, (uint8_t)r); break;
                case 5: r = (unsigned)dstv - srcv; flag_sub8(ctx, dstv, srcv, 0, r); break;
                case 6: r = dstv ^ srcv; flag_logic8(ctx, (uint8_t)r); break;
                default: r = (unsigned)dstv - srcv; flag_sub8(ctx, dstv, srcv, 0, r); break;
            }
            if (kind != 7) { // CMP writes nothing
                if (is_reg) *dptr = (uint8_t)r;
                else x86_mem_write8(ctx, daddr, (uint8_t)r);
            }
            return 0;
        }
        if (form == 4) { // AL, imm8
            uint8_t imm = x86_mem_read8(ctx, ctx->regs.eip++);
            uint8_t a = (uint8_t)ctx->regs.eax;
            unsigned r;
            switch (kind) {
                case 0: r = (unsigned)a + imm; flag_add8(ctx, a, imm, 0, r); ctx->regs.eax = (ctx->regs.eax & 0xFFFFFF00u) | (uint8_t)r; break;
                case 1: r = a | imm; flag_logic8(ctx, (uint8_t)r); ctx->regs.eax = (ctx->regs.eax & 0xFFFFFF00u) | (uint8_t)r; break;
                case 2: r = (unsigned)a + imm + ((ctx->regs.eflags & X86_CF) ? 1 : 0); flag_add8(ctx, a, imm, 0, r); ctx->regs.eax = (ctx->regs.eax & 0xFFFFFF00u) | (uint8_t)r; break;
                case 3: r = (unsigned)a - imm - ((ctx->regs.eflags & X86_CF) ? 1 : 0); flag_sub8(ctx, a, imm, 0, r); ctx->regs.eax = (ctx->regs.eax & 0xFFFFFF00u) | (uint8_t)r; break;
                case 4: r = a & imm; flag_logic8(ctx, (uint8_t)r); ctx->regs.eax = (ctx->regs.eax & 0xFFFFFF00u) | (uint8_t)r; break;
                case 5: r = (unsigned)a - imm; flag_sub8(ctx, a, imm, 0, r); ctx->regs.eax = (ctx->regs.eax & 0xFFFFFF00u) | (uint8_t)r; break;
                case 6: r = a ^ imm; flag_logic8(ctx, (uint8_t)r); ctx->regs.eax = (ctx->regs.eax & 0xFFFFFF00u) | (uint8_t)r; break;
                default: r = (unsigned)a - imm; flag_sub8(ctx, a, imm, 0, r); break;
            }
            return 0;
        }
    }
    // segment push/pop (0x06/07/0E/16/17/1E/1F) - 16-bit segments, no-ops
    if (op == 0x06 || op == 0x07 || op == 0x0E || op == 0x16 || op == 0x17 ||
        op == 0x1E || op == 0x1F) {
        return 0;
    }
    // ---- TEST r/m8, r8 (0x84) ----
    if (op == 0x84) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        uint8_t a, b;
        if (modrm_mod(modrm) == 3) { a = *r8p(ctx, modrm_rm(modrm)); b = *r8p(ctx, modrm_reg(modrm)); }
        else { int c = 0; uint32_t ad = calc_ea(ctx, modrm, &c); ctx->regs.eip += c - 1; a = x86_mem_read8(ctx, ad); b = *r8p(ctx, modrm_reg(modrm)); }
        flag_logic8(ctx, (uint8_t)(a & b));
        return 0;
    }
    // ---- TEST AL, imm8 (0xA8) / TEST EAX, imm32 (0xA9) ----
    if (op == 0xA8) {
        uint8_t imm = x86_mem_read8(ctx, ctx->regs.eip++);
        flag_logic8(ctx, (uint8_t)((uint8_t)ctx->regs.eax & imm));
        return 0;
    }
    if (op == 0xA9) {
        uint32_t imm = read_imm32(ctx);
        x86_update_flags_logic(ctx, ctx->regs.eax & imm);
        return 0;
    }
    // ---- 0x80 group: r/m8, imm8 ----
    if (op == 0x80) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        int ext = modrm_reg(modrm);
        uint8_t imm = x86_mem_read8(ctx, ctx->regs.eip++);
        int is_reg = (modrm_mod(modrm) == 3);
        uint8_t* p = is_reg ? r8p(ctx, modrm_rm(modrm)) : nullptr;
        uint32_t ad = 0;
        if (!is_reg) { int c = 0; ad = calc_ea(ctx, modrm, &c); ctx->regs.eip += c - 1; }
        uint8_t a = is_reg ? *p : x86_mem_read8(ctx, ad);
        unsigned r;
        switch (ext) {
            case 0: r = (unsigned)a + imm; flag_add8(ctx, a, imm, 0, r); break;
            case 1: r = a | imm; flag_logic8(ctx, (uint8_t)r); break;
            case 2: r = (unsigned)a + imm + ((ctx->regs.eflags & X86_CF) ? 1 : 0); flag_add8(ctx, a, imm, 0, r); break;
            case 3: r = (unsigned)a - imm - ((ctx->regs.eflags & X86_CF) ? 1 : 0); flag_sub8(ctx, a, imm, 0, r); break;
            case 4: r = a & imm; flag_logic8(ctx, (uint8_t)r); break;
            case 5: r = (unsigned)a - imm; flag_sub8(ctx, a, imm, 0, r); break;
            case 6: r = a ^ imm; flag_logic8(ctx, (uint8_t)r); break;
            default: r = (unsigned)a - imm; flag_sub8(ctx, a, imm, 0, r); break;
        }
        if (ext != 7) {
            if (is_reg) *p = (uint8_t)r;
            else x86_mem_write8(ctx, ad, (uint8_t)r);
        }
        return 0;
    }
    // ---- Shifts: 0xC0 (r/m8,imm8) 0xD0 (r/m8,1) 0xD2 (r/m8,CL) ----
    if (op == 0xC0 || op == 0xD0 || op == 0xD2) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        int ext = modrm_reg(modrm);
        uint32_t count;
        if (op == 0xC0) count = x86_mem_read8(ctx, ctx->regs.eip++);
        else if (op == 0xD0) count = 1;
        else count = ctx->regs.ecx & 0xFF;
        int is_reg = (modrm_mod(modrm) == 3);
        uint32_t ad = 0;
        if (!is_reg) { int c = 0; ad = calc_ea(ctx, modrm, &c); ctx->regs.eip += c - 1; }
        const uint8_t v = is_reg ? *r8p(ctx, modrm_rm(modrm)) : x86_mem_read8(ctx, ad);
        uint8_t x = (uint8_t)v;
        if (count) {
            count &= 31;
            switch (ext) {
                case 0: x = (uint8_t)((x << count) | (count ? (x >> (8 - count)) : 0)); break; // ROL
                case 1: x = (uint8_t)((x >> count) | (count ? (x << (8 - count)) : 0)); break; // ROR
                case 2: case 3: break; // RCL/RCR approximate as ROL/ROR
                case 4: case 6: { uint8_t cf = x >> (8 - count); x = (uint8_t)(x << count); ctx->regs.eflags = (ctx->regs.eflags & ~X86_CF) | ((cf & 1) ? X86_CF : 0); break; }
                case 5: { uint8_t cf = (x >> (count - 1)) & 1; x = (uint8_t)(x >> count); ctx->regs.eflags = (ctx->regs.eflags & ~X86_CF) | (cf ? X86_CF : 0); break; }
                default: { uint8_t cf = (x >> (count - 1)) & 1; x = (uint8_t)((int8_t)x >> count); ctx->regs.eflags = (ctx->regs.eflags & ~X86_CF) | (cf ? X86_CF : 0); break; }
            }
            flag_szp8(ctx, x);
        }
        if (is_reg) *r8p(ctx, modrm_rm(modrm)) = x; else x86_mem_write8(ctx, ad, x);
        return 0;
    }
    // ---- 0xF6 group: r/m8 ----
    if (op == 0xF6) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        int ext = modrm_reg(modrm);
        int is_reg = (modrm_mod(modrm) == 3);
        uint32_t ad = 0;
        if (!is_reg) { int c = 0; ad = calc_ea(ctx, modrm, &c); ctx->regs.eip += c - 1; }
        uint8_t v = is_reg ? *r8p(ctx, modrm_rm(modrm)) : x86_mem_read8(ctx, ad);
        switch (ext) {
            case 0: { uint8_t imm = x86_mem_read8(ctx, ctx->regs.eip++); flag_logic8(ctx, (uint8_t)(v & imm)); break; }
            case 2: { uint8_t r = (uint8_t)~v; if (is_reg) *r8p(ctx, modrm_rm(modrm)) = r; else x86_mem_write8(ctx, ad, r); break; }
            case 3: { uint8_t r = (uint8_t)(0 - v); flag_sub8(ctx, 0, v, 0, (unsigned)(0 - (int)v)); flag_szp8(ctx, r); if (is_reg) *r8p(ctx, modrm_rm(modrm)) = r; else x86_mem_write8(ctx, ad, r); break; }
            case 4: { uint16_t r = (uint16_t)((uint16_t)ctx->regs.eax * v); ctx->regs.eax = (ctx->regs.eax & 0xFFFF0000u) | (r & 0xFFFF); break; } // MUL
            case 5: { int16_t r = (int16_t)((int8_t)ctx->regs.eax * (int8_t)v); ctx->regs.eax = (ctx->regs.eax & 0xFFFF0000u) | ((uint16_t)r & 0xFFFF); break; } // IMUL
            case 6: { if (v) { uint16_t ax = (uint16_t)ctx->regs.eax; ctx->regs.eax = (ctx->regs.eax & 0xFFFF0000u) | (ax / v); } break; } // DIV
            case 7: { if (v) { int16_t ax = (int16_t)ctx->regs.eax; int16_t q = (int16_t)(ax / (int8_t)v); ctx->regs.eax = (ctx->regs.eax & 0xFFFF0000u) | ((uint16_t)q & 0xFFFF); } break; } // IDIV
            default: break;
        }
        return 0;
    }
    // ---- PUSHF / POPF (0x9C / 0x9D) ----
    if (op == 0x9C) { ctx->regs.esp -= 4; x86_mem_write32(ctx, ctx->regs.esp, ctx->regs.eflags); return 0; }
    if (op == 0x9D) { ctx->regs.eflags = x86_mem_read32(ctx, ctx->regs.esp) | 0x00000002; ctx->regs.esp += 4; return 0; }
    // ---- PUSHA / POPA (0x60 / 0x61) ----
    if (op == 0x60) {
        uint32_t sp = ctx->regs.esp;
        ctx->regs.esp -= 4; x86_mem_write32(ctx, ctx->regs.esp, ctx->regs.eax);
        ctx->regs.esp -= 4; x86_mem_write32(ctx, ctx->regs.esp, ctx->regs.ecx);
        ctx->regs.esp -= 4; x86_mem_write32(ctx, ctx->regs.esp, ctx->regs.edx);
        ctx->regs.esp -= 4; x86_mem_write32(ctx, ctx->regs.esp, ctx->regs.ebx);
        ctx->regs.esp -= 4; x86_mem_write32(ctx, ctx->regs.esp, sp);
        ctx->regs.esp -= 4; x86_mem_write32(ctx, ctx->regs.esp, ctx->regs.ebp);
        ctx->regs.esp -= 4; x86_mem_write32(ctx, ctx->regs.esp, ctx->regs.esi);
        ctx->regs.esp -= 4; x86_mem_write32(ctx, ctx->regs.esp, ctx->regs.edi);
        return 0;
    }
    if (op == 0x61) {
        ctx->regs.edi = x86_mem_read32(ctx, ctx->regs.esp); ctx->regs.esp += 4;
        ctx->regs.esi = x86_mem_read32(ctx, ctx->regs.esp); ctx->regs.esp += 4;
        ctx->regs.ebp = x86_mem_read32(ctx, ctx->regs.esp); ctx->regs.esp += 4;
        ctx->regs.esp += 4; // skip saved SP
        ctx->regs.ebx = x86_mem_read32(ctx, ctx->regs.esp); ctx->regs.esp += 4;
        ctx->regs.edx = x86_mem_read32(ctx, ctx->regs.esp); ctx->regs.esp += 4;
        ctx->regs.ecx = x86_mem_read32(ctx, ctx->regs.esp); ctx->regs.esp += 4;
        ctx->regs.eax = x86_mem_read32(ctx, ctx->regs.esp); ctx->regs.esp += 4;
        return 0;
    }

    // ---- XCHG r/m8, r8 (0x86) ----
    if (op == 0x86) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        uint8_t* r = r8p(ctx, modrm_reg(modrm));
        if (modrm_mod(modrm) == 3) { uint8_t* p = r8p(ctx, modrm_rm(modrm)); uint8_t t = *p; *p = *r; *r = t; }
        else { int c = 0; uint32_t ad = calc_ea(ctx, modrm, &c); ctx->regs.eip += c - 1; uint8_t t = x86_mem_read8(ctx, ad); x86_mem_write8(ctx, ad, *r); *r = t; }
        return 0;
    }
    // ---- MOV r/m16, Sreg (0x8C) / MOV Sreg, r/m16 (0x8E) ----
    if (op == 0x8C || op == 0x8E) {
        uint8_t modrm = x86_mem_read8(ctx, ctx->regs.eip++);
        int sreg = modrm_reg(modrm);
        uint16_t val = 0;
        if (op == 0x8C) {
            switch (sreg) {
                case 0: val = ctx->regs.es; break;
                case 1: val = ctx->regs.cs; break;
                case 2: val = ctx->regs.ss; break;
                case 3: val = ctx->regs.ds; break;
                case 4: val = ctx->regs.fs; break;
                case 5: val = ctx->regs.gs; break;
            }
            if (modrm_mod(modrm) == 3) {
                uint32_t* d = x86_reg_ptr(ctx, modrm_rm(modrm));
                *d = (*d & 0xFFFF0000u) | val;
            } else {
                int c = 0; uint32_t ad = calc_ea(ctx, modrm, &c); ctx->regs.eip += c - 1;
                x86_mem_write16(ctx, ad, val);
            }
        } else {
            if (modrm_mod(modrm) == 3) val = (uint16_t)*x86_reg_ptr(ctx, modrm_rm(modrm));
            else { int c = 0; uint32_t ad = calc_ea(ctx, modrm, &c); ctx->regs.eip += c - 1; val = x86_mem_read16(ctx, ad); }
            switch (sreg) {
                case 0: ctx->regs.es = val; break;
                case 2: ctx->regs.ss = val; break;
                case 3: ctx->regs.ds = val; break;
                case 4: ctx->regs.fs = val; break;
                case 5: ctx->regs.gs = val; break;
                default: break;
            }
        }
        return 0;
    }
    // ---- FWAIT (0x9B), LAHF (0x9F), SAHF (0x9E) ----
    if (op == 0x9B) return 0;
    if (op == 0x9F) {
        ctx->regs.eax = (ctx->regs.eax & 0xFFFFFF00u) | (uint8_t)((ctx->regs.eflags & 0xD5) | 2);
        return 0;
    }
    if (op == 0x9E) {
        uint8_t ah = (uint8_t)(ctx->regs.eax >> 8);
        ctx->regs.eflags = (ctx->regs.eflags & ~0xD5u) | (ah & 0xD5);
        return 0;
    }
    // ---- XLATB (0xD7) ----
    if (op == 0xD7) {
        uint8_t v = x86_mem_read8(ctx, ctx->regs.ebx + (ctx->regs.eax & 0xFF));
        ctx->regs.eax = (ctx->regs.eax & 0xFFFFFF00u) | v;
        return 0;
    }
    // ---- LOOPNE/LOOPE/LOOP (0xE0-0xE2) and JECXZ (0xE3) ----
    if (op >= 0xE0 && op <= 0xE2) {
        int8_t rel = (int8_t)x86_mem_read8(ctx, ctx->regs.eip++);
        ctx->regs.ecx--;
        int zf = (ctx->regs.eflags & X86_ZF) != 0;
        int take = (op == 0xE2) ? (ctx->regs.ecx != 0)
                 : (op == 0xE1) ? (ctx->regs.ecx != 0 && zf)
                 : (ctx->regs.ecx != 0 && !zf); // LOOPNE
        if (take) ctx->regs.eip += rel;
        return 0;
    }
    if (op == 0xE3) {
        int8_t rel = (int8_t)x86_mem_read8(ctx, ctx->regs.eip++);
        if (ctx->regs.ecx == 0) ctx->regs.eip += rel;
        return 0;
    }

    // ---- Unknown instruction ----
    char buf[256];
    snprintf(buf, sizeof(buf), "x86: UNHANDLED opcode 0x%02X at EIP=0x%08X (insn #%llu)",
             op, eip_start, (unsigned long long)ctx->insn_count);
    if (ctx->log) ctx->log(buf);
    ctx->running = 0;
    ctx->execution = X86_EXEC_BAD_INSTRUCTION; // illegal instruction
    return -1;
}

// ---- Memory management ----
// One contiguous 4GB reservation makes every guest VA a valid host pointer
// (base + VA), which is what the 64-bit bridges dereference.
#if defined(_WIN32)
#include <windows.h>
static void* xg_reserve4g(size_t size) {
    return VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS);
}
static int xg_commit_pages(void* base, size_t off, size_t size) {
    return VirtualAlloc((char*)base + off, size, MEM_COMMIT, PAGE_READWRITE) ? 0 : -1;
}
static void xg_release4g(void* base, size_t size) {
    if (base) VirtualFree(base, 0, MEM_RELEASE);
    (void)size;
}
#else
#include <sys/mman.h>
static void* xg_reserve4g(size_t size) {
    void* p = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return (p == MAP_FAILED) ? nullptr : p;
}
static int xg_commit_pages(void* base, size_t off, size_t size) {
    return mprotect((char*)base + off, size, PROT_READ | PROT_WRITE);
}
static void xg_release4g(void* base, size_t size) {
    if (base) munmap(base, size);
}
#endif

static int xg_ensure_reserved(x86_context* ctx) {
    if (ctx->mem.base) return 0;
    ctx->mem.base = (uint8_t*)xg_reserve4g((size_t)X86_PAGE_COUNT * X86_PAGE_SIZE);
    if (!ctx->mem.base) return -1;
    // Commit the entire 4 GB reservation up front so that guest writes (and
    // host bridges writing through guest pointers) never SIGSEGV on an
    // uncommitted page. mprotect on the whole range is a single syscall and
    // allocates no physical memory (pages are demand-zeroed by the kernel).
    if (xg_commit_pages(ctx->mem.base, 0, (size_t)X86_PAGE_COUNT * X86_PAGE_SIZE) != 0)
        return -1;
    // Mark every page as committed in the bitmap.
    for (size_t i = 0; i < sizeof(ctx->mem.map) / sizeof(ctx->mem.map[0]); i++)
        ctx->mem.map[i] = ~0ULL;
    ctx->mem.committed = (size_t)X86_PAGE_COUNT * X86_PAGE_SIZE;
    return 0;
}

int x86_mem_commit(x86_context* ctx, uint32_t base, size_t size) {
    if (xg_ensure_reserved(ctx) != 0) return -1;
    uint32_t start_page = base >> X86_PAGE_SHIFT;
    uint32_t end_page = (base + (uint32_t)size + X86_PAGE_SIZE - 1) >> X86_PAGE_SHIFT;
    if (end_page > X86_PAGE_COUNT) return -1;

    // Commit in page-sized runs to keep mprotect/VirtualAlloc calls few.
    uint32_t p = start_page;
    while (p < end_page) {
        if (x86_page_mapped(ctx, p)) { ++p; continue; }
        uint32_t run = p;
        while (run < end_page && !x86_page_mapped(ctx, run)) ++run;
        size_t off = (size_t)p * X86_PAGE_SIZE;
        size_t len = (size_t)(run - p) * X86_PAGE_SIZE;
        if (xg_commit_pages(ctx->mem.base, off, len) != 0) return -1;
        memset(ctx->mem.base + off, 0, len); // anonymous pages are zero; enforce it
        for (uint32_t q = p; q < run; q++) ctx->mem.map[q >> 6] |= (1ull << (q & 63));
        ctx->mem.committed += len;
        p = run;
    }
    return 0;
}

void x86_mem_free(x86_context* ctx) {
    if (ctx->mem.base) {
        xg_release4g(ctx->mem.base, (size_t)X86_PAGE_COUNT * X86_PAGE_SIZE);
        ctx->mem.base = nullptr;
    }
    memset(ctx->mem.map, 0, sizeof(ctx->mem.map));
    ctx->mem.committed = 0;
}

int x86_mem_map_host(x86_context* ctx, uint32_t guest_addr, void* host_ptr, size_t size) {
    if (x86_mem_commit(ctx, guest_addr, size) != 0) return -1;
    memcpy(ctx->mem.base + guest_addr, host_ptr, size);
    x86_jit_flush(ctx);
    return 0;
}

// ---- CPU lifecycle ----
void x86_cpu_init(x86_context* ctx) {
    void* old_base = ctx->mem.base; // preserve an existing reservation to free
    // A re-init must not silence the caller's log: x86_run_pe32() calls this
    // itself when the context has no reservation yet, and a host that set the
    // hook before that call would otherwise lose every diagnostic line.
    void (*old_log)(const char*) = ctx->log;
    memset(ctx, 0, sizeof(*ctx));
    ctx->log = old_log;
    // A new guest starts with an empty arena: the previous guest's blocks
    // pointed into a reservation that is about to be released.
    x86_guest_heap_reset();
    if (old_base) xg_release4g(old_base, (size_t)X86_PAGE_COUNT * X86_PAGE_SIZE);
    ctx->regs.esp = 0x00100000; // 1MB stack
    ctx->regs.fpu_top = 0;
    ctx->regs.fpu_cw = 0x037F; // default control word
    ctx->regs.fpu_sw = 0;
    for (int i = 0; i < 8; i++) { ctx->regs.fpu_tags[i] = 3; } // all empty
    ctx->regs.ebp = ctx->regs.esp;
    ctx->regs.eflags = 0x00000202; // IF set
    ctx->regs.cs = 0x1B;
    ctx->regs.ds = 0x23;
    ctx->regs.es = 0x23;
    ctx->regs.fs = 0x3B;
    ctx->regs.fs_base = 0x7FFDE000; // TEB base
    ctx->regs.gs = 0x00;
    ctx->regs.ss = 0x23;
    ctx->insn_limit = 100000000; // 100M instructions max
    ctx->running = 0;

    // ---- TEB + TLS array - Making FS:[...] accesses valid memory ----
    if (x86_mem_commit(ctx, 0x7FFDE000, 0x2000) == 0) {
        // TEB fields the CRT reads
        x86_mem_write32(ctx, 0x7FFDE000 + 0x00, 0xFFFFFFFF); // SEH chain end
        x86_mem_write32(ctx, 0x7FFDE000 + 0x04, 0x00100000); // stack base
        x86_mem_write32(ctx, 0x7FFDE000 + 0x08, 0x00100000); // stack limit
        x86_mem_write32(ctx, 0x7FFDE000 + 0x18, 0x7FFDE000); // Self
        x86_mem_write32(ctx, 0x7FFDE000 + 0x20, 1);          // ProcessId
        x86_mem_write32(ctx, 0x7FFDE000 + 0x24, 1);          // ThreadId
    }
    // TLS pointer table (64 slots of per-module blocks)
    ctx->tls_array = 0x7FFE0000;
    if (x86_mem_commit(ctx, ctx->tls_array, 0x1000) == 0) {
        x86_mem_write32(ctx, 0x7FFDE000 + 0x2C, ctx->tls_array);
    }
    ctx->regs.tsc_base = 0x100000;

    // ---- Basic-block recompiler ----
    // Enable the basic-block fast path only when requested. The corpus
    // uses the interpreter by default so new instructions fail visibly.
    const char* jit = getenv("TRT_X86_JIT");
    ctx->jit_enabled = (jit && jit[0] == '1');
    ctx->code_epoch = 1;

    // Sentinel page: the SEH dispatcher calls guest handlers with this address
    // as the return address, and HLT stops the nested run loop there.
    if (x86_mem_commit(ctx, 0x7FFD0000, 0x1000) == 0) {
        x86_mem_write8(ctx, 0x7FFD0000, 0xF4);
        ctx->seh_sentinel = 0x7FFD0000;
    }
}

void x86_cpu_reset(x86_context* ctx) {
    x86_mem_free(ctx);
    x86_cpu_init(ctx);
}

int x86_run(x86_context* ctx) {
    ctx->running = 1;
    ctx->execution = X86_EXEC_OK;
    // Resolved once per run: a stalled window is reported by name instead of
    // ending as a bare instruction-limit exhaustion.
    const uint64_t hot_limit = hotloop_limit_from_env();
    eip_profile_reset();
    g_hot = {};
    g_x86_last_import[0] = 0;
    while (ctx->running && ctx->execution == X86_EXEC_OK) {
        // Run a recompiled basic block when one covers this address; otherwise
        // interpret exactly one instruction (which may itself end a block).
        int r = x86_jit_step(ctx);
        if (r < 0) {
            if (ctx->execution == X86_EXEC_OK) ctx->execution = X86_EXEC_JIT_FAILURE;
            break;
        }
        if (r == 0 && x86_step(ctx) != 0) {
            if (ctx->execution == X86_EXEC_OK) ctx->execution = X86_EXEC_BAD_INSTRUCTION;
            break;
        }
        hotloop_tick(ctx, hot_limit);
        if ((ctx->insn_count & 63u) == 0) {
            eip_profile_sample(ctx->regs.eip);
        }
    }
    ctx->running = 0;
    eip_profile_report(ctx);
    return ctx->execution;
}

// Install the main image's static TLS before process-attach callbacks and
// CRT startup. FS:[0x2c] points to a vector of 32-bit guest TLS addresses.
static bool install_main_tls(x86_context* ctx, const trt_image& image) {
    if (!image.tls_data_rva && !image.tls_index_rva && !image.tls_zero_fill)
        return true;
    const uint64_t size = uint64_t(image.tls_data_size) + image.tls_zero_fill;
    if (size > UINT32_MAX || uint64_t(image.tls_index_rva) + 4 > ctx->image_size ||
        uint64_t(image.tls_data_rva) + image.tls_data_size > ctx->image_size) return false;
    const uint32_t block = x86_guest_alloc(ctx, size ? uint32_t(size) : 1, 1);
    if (!block) return false;
    memcpy(ctx->mem.base + block, ctx->mem.base + ctx->image_va + image.tls_data_rva,
           image.tls_data_size);
    constexpr uint32_t main_index = 0;
    x86_mem_write32(ctx, ctx->image_va + image.tls_index_rva, main_index);
    x86_mem_write32(ctx, ctx->tls_array + main_index * 4, block);
    return true;
}

// ---- Run a 32-bit PE through the interpreter ----
int x86_run_pe32(x86_context* ctx, void* image_base, size_t image_size,
                 uint32_t guest_base, uint32_t entry_rva, uint32_t stack_base,
                 size_t stack_size, const trt_image* image) {
    // The caller normally initializes first (to install the log hook); only
    // initialize when it did not, so a second call never leaks the 4GB
    // reservation.
    if (!ctx->mem.base) x86_cpu_init(ctx);

    // Map the PE image at the guest VA base the loader relocated it for.
    if (x86_mem_map_host(ctx, guest_base, image_base, image_size) != 0) {
        if (ctx->log) ctx->log("x86: failed to map PE image");
        return -1;
    }
    // Commit a tail past SizeOfImage. Host libc routines that bridges call on
    // guest pointers (glibc's AVX memset/memcpy) may store a few bytes beyond
    // the requested length, assuming the surrounding memory is mapped. A guest
    // object ending exactly at SizeOfImage - page aligned - would otherwise
    // fault inside the bridge on the uncommitted page after the image.
    if (x86_mem_commit(ctx, guest_base + (uint32_t)image_size, 0x400000) != 0) {
        if (ctx->log) ctx->log("x86: failed to commit image tail");
        return -1;
    }

    ctx->image_base = image_base;
    ctx->image_size = (uint32_t)image_size;
    ctx->image_va = guest_base;  // what GetModuleHandle(NULL) must answer
    // A PE32 guest uses the 24-byte i386 CRITICAL_SECTION layout: bridges that
    // initialize one must not write the Win64 40-byte struct over it.
    // Set up stack
    if (stack_size < 16 || uint64_t(stack_base) + stack_size > UINT32_MAX ||
        x86_mem_commit(ctx, stack_base, stack_size) != 0) {
        if (ctx->log) ctx->log("x86: failed to commit stack");
        return -1;
    }
    const uint32_t stack_top = stack_base + static_cast<uint32_t>(stack_size);
    x86_mem_write32(ctx, ctx->regs.fs_base + 0x04, stack_top);
    x86_mem_write32(ctx, ctx->regs.fs_base + 0x08, stack_base);
    if (!ctx->seh_sentinel) {
        if (ctx->log) ctx->log("x86: could not reserve an entry return sentinel");
        return X86_EXEC_MEMORY;
    }
    if (image && !install_main_tls(ctx, *image)) {
        if (ctx->log) ctx->log("x86: could not initialize static TLS for the main image");
        return X86_EXEC_MEMORY;
    }

    // Use PE32 argument widths for bridge calls even during TLS callbacks.
    struct pointer_width_scope {
        int previous = trt_cs_size_for_guest() == 24 ? 32 : 64;
        pointer_width_scope() { trt_set_guest_ptr_bits(32); }
        ~pointer_width_scope() { trt_set_guest_ptr_bits(previous); }
    } pointer_width;
    ctx->insn_count = 0;
    if (image) {
        for (int index = 0; index < image->tls_callback_count; ++index) {
            const auto callback = image->tls_callback_rvas[index];
            if (callback >= image_size) return X86_EXEC_BAD_INSTRUCTION;
            ctx->regs.esp = stack_top - 16;
            ctx->regs.ebp = ctx->regs.esp;
            x86_mem_write32(ctx, stack_top - 16, ctx->seh_sentinel);
            x86_mem_write32(ctx, stack_top - 12, guest_base);
            x86_mem_write32(ctx, stack_top - 8, 1); // DLL_PROCESS_ATTACH
            x86_mem_write32(ctx, stack_top - 4, 0);
            ctx->regs.eip = guest_base + static_cast<uint32_t>(callback);
            const int result = x86_run(ctx);
            if (result != X86_EXEC_OK) return result;
        }
    }

    ctx->regs.esp = stack_top - 4;
    ctx->regs.ebp = ctx->regs.esp;
    x86_mem_write32(ctx, ctx->regs.esp, ctx->seh_sentinel);
    ctx->regs.eip = guest_base + entry_rva;
    return x86_run(ctx);
}
