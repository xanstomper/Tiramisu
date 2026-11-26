// tiramisu/src/trt/x86_jit.cpp
// Basic-block recompiler for the 32-bit guest CPU.
//
// x86_run() used to re-read and re-decode every instruction from guest memory
// on every step. This unit decodes a straight-line run of guest instructions
// ONCE into a compact internal form and executes that form until the block
// ends (jump / call / ret / unsupported opcode). EIP is only written when the
// block leaves, so the common case costs one array walk with no guest fetch.
//
// Correctness rules:
//   * An opcode this unit does not model ends the block BEFORE it. The next
//     x86_step() call interprets it, so the recompiler can never change the
//     semantics of an instruction it does not understand.
//   * Every store into a page that hosts recompiled code bumps code_epoch,
//     which invalidates all blocks (self-modifying code).
//   * 32-bit flags come from the same helpers the interpreter uses.
#include "x86_cpu.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" int trt_pe32_is_trap(uint32_t guest_va);

// ---- Opcode kinds -------------------------------------------------------
enum {
    J_NOP = 0,
    J_MOV_RR, J_MOV_RM, J_MOV_MR, J_MOV_RI,
    J_ALU_RR, J_ALU_RM, J_ALU_MR, J_ALU_RI,
    J_TEST_RR, J_TEST_RM,
    J_LEA,
    J_INC, J_DEC,
    J_PUSH_R, J_POP_R, J_PUSH_I, J_PUSH_RM,
    J_MOVZX8, J_MOVZX16, J_MOVSX8, J_MOVSX16,
    J_MOV8_RR, J_MOV8_RM, J_MOV8_MR, J_MOV8_RI,
    J_ALU8_RR, J_ALU8_RM, J_ALU8_MR,
    J_SETCC, J_CMOVCC,
    J_JMP, J_JCC, J_JMP_R, J_CALL_R, J_CALL_REL, J_RET, J_RET_I,
    J_HLT
};

enum { JIT_MAX_OPS = 64 };
enum { JIT_BUCKETS = 1024 };
enum { JIT_NO_REG = 0xFF };

// One decoded guest instruction.
struct jit_op {
    uint8_t kind;
    uint8_t sub;      // ALU sub-op, or condition code for JCC/SETCC/CMOVCC
    uint8_t dst;      // destination register (JIT_NO_REG when the EA is the target)
    uint8_t src;      // source register
    uint8_t rm;       // ModRM r/m register (register form)
    uint8_t is_mem;   // r/m operand is memory
    uint8_t ea_base;  // JIT_NO_REG when absent
    uint8_t ea_idx;   // JIT_NO_REG when absent
    uint8_t ea_scale; // 1/2/4/8
    uint8_t ea_fs;    // FS segment override
    int32_t ea_disp;
    uint32_t imm;
    uint32_t target;
};

struct jit_block {
    uint32_t start;   // guest VA of the first instruction (JIT_EMPTY when unused)
    uint32_t end;     // guest VA just past the last instruction
    uint32_t epoch;   // code_epoch this block was built against
    uint32_t nops;
    jit_op ops[JIT_MAX_OPS];
};

struct jit_cache {
    jit_block* buckets[JIT_BUCKETS];
    unsigned long long blocks, insns, decoded, evictions;
};

#define JIT_EMPTY 0xFFFFFFFFu

// ---- Decoder ------------------------------------------------------------
static inline int jit_rd8(x86_context* c, uint32_t a) {
    return (int)x86_mem_read8(c, a);
}
static inline uint32_t jit_rd32(x86_context* c, uint32_t a) {
    return x86_mem_read32(c, a);
}

// Parse ModRM/SIB/disp starting at `p` (the ModRM byte). Fills EA fields and
// op->rm, and returns the bytes consumed (>=1), or 0 when the encoding runs
// off committed memory.
static int jit_parse_modrm(x86_context* c, uint32_t p, jit_op* op) {
    uint8_t m = (uint8_t)jit_rd8(c, p);
    int mod = (m >> 6) & 3;
    int rm = m & 7;
    op->ea_base = JIT_NO_REG;
    op->ea_idx = JIT_NO_REG;
    op->ea_scale = 1;
    op->ea_disp = 0;
    op->rm = (uint8_t)rm;

    if (mod == 3) {
        op->is_mem = 0;
        return 1;
    }
    op->is_mem = 1;

    int n = 1;
    if (rm == 4) {
        uint8_t sib = (uint8_t)jit_rd8(c, p + 1);
        n = 2;
        int sc = (sib >> 6) & 3, ix = (sib >> 3) & 7, bs = sib & 7;
        if (ix != 4) { op->ea_idx = (uint8_t)ix; op->ea_scale = (uint8_t)(1 << sc); }
        if (bs == 5 && mod == 0) {
            op->ea_disp = (int32_t)jit_rd32(c, p + 2);
            n += 4;
        } else {
            op->ea_base = (uint8_t)bs;
        }
    } else if (mod == 0 && rm == 5) {
        op->ea_disp = (int32_t)jit_rd32(c, p + 1);
        n += 4;
    } else {
        op->ea_base = (uint8_t)rm;
    }

    if (mod == 1) {
        op->ea_disp += (int8_t)(uint8_t)jit_rd8(c, p + n);
        n += 1;
    } else if (mod == 2) {
        op->ea_disp += (int32_t)jit_rd32(c, p + n);
        n += 4;
    }
    return n;
}

// Decode the instruction at guest VA `va`. Returns its length, or 0 when the
// encoding is outside this recompiler's model (caller falls back to x86_step).
static int jit_decode(x86_context* c, uint32_t va, jit_op* op) {
    memset(op, 0, sizeof(*op));
    op->ea_base = JIT_NO_REG;
    op->ea_idx = JIT_NO_REG;
    op->ea_scale = 1;
    op->dst = JIT_NO_REG;
    op->src = JIT_NO_REG;
    op->rm = JIT_NO_REG;

    // x86 instructions are at most 15 bytes. Requiring the whole 16-byte span
    // to be committed keeps every decode read inside real guest memory.
    if (!x86_span_mapped(c, va, 16)) return 0;

    uint32_t p = va;
    int fs = 0;
    uint8_t b = (uint8_t)jit_rd8(c, p);

    // Segment prefixes are modelled; everything else (operand/address size,
    // REP, LOCK, CS/DS/SS/ES overrides) falls back to the interpreter.
    while (b == 0x64 || b == 0x65) {
        fs = (b == 0x64);
        p++;
        b = (uint8_t)jit_rd8(c, p);
    }
    op->ea_fs = (uint8_t)fs;

    uint8_t opcode = b;
    p++;

    // ---- one-byte opcodes ----
    if (opcode == 0x90) { op->kind = J_NOP; return (int)(p - va); }
    if (opcode == 0xF4) { op->kind = J_HLT; return (int)(p - va); }

    if (opcode >= 0x40 && opcode <= 0x47) { op->kind = J_INC; op->dst = opcode - 0x40; return (int)(p - va); }
    if (opcode >= 0x48 && opcode <= 0x4F) { op->kind = J_DEC; op->dst = opcode - 0x48; return (int)(p - va); }
    if (opcode >= 0x50 && opcode <= 0x57) { op->kind = J_PUSH_R; op->dst = opcode - 0x50; return (int)(p - va); }
    if (opcode >= 0x58 && opcode <= 0x5F) { op->kind = J_POP_R;  op->dst = opcode - 0x58; return (int)(p - va); }

    if (opcode == 0x68) { op->kind = J_PUSH_I; op->imm = jit_rd32(c, p); return (int)(p + 4 - va); }
    if (opcode == 0x6A) {
        op->kind = J_PUSH_I;
        op->imm = (uint32_t)(int32_t)(int8_t)(uint8_t)jit_rd8(c, p);
        return (int)(p + 1 - va);
    }

    if (opcode >= 0xB8 && opcode <= 0xBF) {
        op->kind = J_MOV_RI;
        op->dst = opcode - 0xB8;
        op->imm = jit_rd32(c, p);
        return (int)(p + 4 - va);
    }

    // MOV r/m8,r8 (88), MOV r/m32,r32 (89), MOV r8,r/m8 (8A), MOV r32,r/m32 (8B)
    if (opcode == 0x88 || opcode == 0x89 || opcode == 0x8A || opcode == 0x8B) {
        uint8_t m = (uint8_t)jit_rd8(c, p);
        int n = jit_parse_modrm(c, p, op);
        if (!n) return 0;
        int reg = (m >> 3) & 7;
        int byte = (opcode == 0x88 || opcode == 0x8A);
        int to_reg = (opcode == 0x8A || opcode == 0x8B);
        op->src = (uint8_t)reg;
        if (to_reg) {
            op->kind = byte ? (op->is_mem ? J_MOV8_RM : J_MOV8_RR)
                            : (op->is_mem ? J_MOV_RM : J_MOV_RR);
            op->dst = (uint8_t)reg;
            if (!op->is_mem && !byte) op->src = op->rm; // dst <- rm
            if (!op->is_mem && byte) op->src = op->rm;
        } else {
            op->kind = byte ? J_MOV8_MR : J_MOV_MR;
            if (!op->is_mem) {
                op->dst = op->rm;
                op->src = (uint8_t)reg;
            }
        }
        return (int)(p + n - va);
    }

    if (opcode == 0x8D) { // LEA (memory form only)
        uint8_t m = (uint8_t)jit_rd8(c, p);
        int n = jit_parse_modrm(c, p, op);
        if (!n || !op->is_mem) return 0;
        op->kind = J_LEA;
        op->dst = (uint8_t)((m >> 3) & 7);
        return (int)(p + n - va);
    }

    // ADD/OR/AND/SUB/XOR/CMP r/m,r and r,r/m (ADC/SBB need carry-in: fallback)
    if (opcode == 0x01 || opcode == 0x03 || opcode == 0x21 || opcode == 0x23 ||
        opcode == 0x29 || opcode == 0x2B || opcode == 0x31 || opcode == 0x33 ||
        opcode == 0x39 || opcode == 0x3B) {
        uint8_t m = (uint8_t)jit_rd8(c, p);
        int n = jit_parse_modrm(c, p, op);
        if (!n) return 0;
        int sub = (opcode >> 3) & 7;
        int reg = (m >> 3) & 7;
        int to_reg = (opcode & 2) != 0;
        op->sub = (uint8_t)sub;
        if (to_reg) {
            if (op->is_mem) { op->kind = J_ALU_RM; op->dst = (uint8_t)reg; }
            else            { op->kind = J_ALU_RR; op->dst = (uint8_t)reg; op->src = op->rm; }
        } else {
            if (op->is_mem) { op->kind = J_ALU_MR; op->src = (uint8_t)reg; }
            else            { op->kind = J_ALU_RR; op->dst = op->rm; op->src = (uint8_t)reg; }
        }
        return (int)(p + n - va);
    }

    if (opcode == 0x85) { // TEST r/m32, r32
        uint8_t m = (uint8_t)jit_rd8(c, p);
        int n = jit_parse_modrm(c, p, op);
        if (!n) return 0;
        int reg = (m >> 3) & 7;
        if (op->is_mem) { op->kind = J_TEST_RM; op->dst = (uint8_t)reg; }
        else { op->kind = J_TEST_RR; op->dst = op->rm; op->src = (uint8_t)reg; }
        return (int)(p + n - va);
    }

    // Group 1: ALU r/m32, imm32 (81) / imm8 sign-extended (83)
    if (opcode == 0x81 || opcode == 0x83) {
        uint8_t m = (uint8_t)jit_rd8(c, p);
        int sub = (m >> 3) & 7;
        if (sub == 2 || sub == 3) return 0; // ADC/SBB
        int n = jit_parse_modrm(c, p, op);
        if (!n) return 0;
        op->kind = J_ALU_RI;
        op->sub = (uint8_t)sub;
        if (!op->is_mem) op->dst = op->rm;
        if (opcode == 0x81) { op->imm = jit_rd32(c, p + n); n += 4; }
        else { op->imm = (uint32_t)(int32_t)(int8_t)(uint8_t)jit_rd8(c, p + n); n += 1; }
        return (int)(p + n - va);
    }

    if (opcode == 0xC7) { // MOV r/m32, imm32 (/0)
        uint8_t m = (uint8_t)jit_rd8(c, p);
        if (((m >> 3) & 7) != 0) return 0;
        int n = jit_parse_modrm(c, p, op);
        if (!n) return 0;
        op->kind = J_MOV_RI;
        op->imm = jit_rd32(c, p + n);
        n += 4;
        if (!op->is_mem) op->dst = op->rm;
        return (int)(p + n - va);
    }

    if (opcode == 0xC6) { // MOV r/m8, imm8 (/0)
        uint8_t m = (uint8_t)jit_rd8(c, p);
        if (((m >> 3) & 7) != 0) return 0;
        int n = jit_parse_modrm(c, p, op);
        if (!n) return 0;
        op->kind = J_MOV8_RI;
        op->imm = (uint32_t)(uint8_t)jit_rd8(c, p + n);
        n += 1;
        if (!op->is_mem) op->dst = op->rm;
        return (int)(p + n - va);
    }

    if (opcode == 0xE9) { op->kind = J_JMP; op->target = p + 4 + jit_rd32(c, p); return (int)(p + 4 - va); }
    if (opcode == 0xEB) { op->kind = J_JMP; op->target = p + 1 + (uint32_t)(int32_t)(int8_t)(uint8_t)jit_rd8(c, p); return (int)(p + 1 - va); }
    if (opcode == 0xE8) { op->kind = J_CALL_REL; op->target = p + 4 + jit_rd32(c, p); return (int)(p + 4 - va); }

    if (opcode == 0xC3) { op->kind = J_RET; return (int)(p - va); }
    if (opcode == 0xC2) { op->kind = J_RET_I; op->imm = x86_mem_read16(c, p); return (int)(p + 2 - va); }

    if (opcode >= 0x70 && opcode <= 0x7F) {
        op->kind = J_JCC;
        op->sub = opcode - 0x70;
        op->target = p + 1 + (uint32_t)(int32_t)(int8_t)(uint8_t)jit_rd8(c, p);
        return (int)(p + 1 - va);
    }

    if (opcode == 0xFF) {
        uint8_t m = (uint8_t)jit_rd8(c, p);
        int ext = (m >> 3) & 7;
        if (ext != 2 && ext != 4 && ext != 6) return 0;
        int n = jit_parse_modrm(c, p, op);
        if (!n) return 0;
        if (ext == 6) op->kind = J_PUSH_RM;
        else if (ext == 4) op->kind = J_JMP_R;
        else op->kind = J_CALL_R;
        return (int)(p + n - va);
    }

    // ---- two-byte (0F) opcodes ----
    if (opcode == 0x0F) {
        uint8_t op2 = (uint8_t)jit_rd8(c, p);
        p++;

        if (op2 >= 0x80 && op2 <= 0x8F) {
            op->kind = J_JCC;
            op->sub = op2 - 0x80;
            op->target = p + 4 + jit_rd32(c, p);
            return (int)(p + 4 - va);
        }

        if (op2 == 0xB6 || op2 == 0xB7 || op2 == 0xBE || op2 == 0xBF) {
            uint8_t m = (uint8_t)jit_rd8(c, p);
            int n = jit_parse_modrm(c, p, op);
            if (!n) return 0;
            op->dst = (uint8_t)((m >> 3) & 7);
            if (op2 == 0xB6) op->kind = J_MOVZX8;
            else if (op2 == 0xB7) op->kind = J_MOVZX16;
            else if (op2 == 0xBE) op->kind = J_MOVSX8;
            else op->kind = J_MOVSX16;
            return (int)(p + n - va);
        }

        if (op2 >= 0x40 && op2 <= 0x4F) { // CMOVcc r32, r/m32
            uint8_t m = (uint8_t)jit_rd8(c, p);
            int n = jit_parse_modrm(c, p, op);
            if (!n) return 0;
            op->kind = J_CMOVCC;
            op->sub = op2 - 0x40;
            op->dst = (uint8_t)((m >> 3) & 7);
            return (int)(p + n - va);
        }

        if (op2 >= 0x90 && op2 <= 0x9F) { // SETcc r/m8
            int n = jit_parse_modrm(c, p, op);
            if (!n) return 0;
            op->kind = J_SETCC;
            op->sub = op2 - 0x90;
            if (!op->is_mem) op->dst = op->rm;
            return (int)(p + n - va);
        }

        // SSE / MMX / CPUID / RDTSC / fences / long NOP etc. stay with the
        // interpreter; end the block here.
        return 0;
    }

    return 0;
}

// ---- Execution helpers --------------------------------------------------
static inline uint8_t* jit_r8(x86_context* c, int i) {
    switch (i) {
        case 0: return (uint8_t*)&c->regs.eax;
        case 1: return (uint8_t*)&c->regs.ecx;
        case 2: return (uint8_t*)&c->regs.edx;
        case 3: return (uint8_t*)&c->regs.ebx;
        case 4: return ((uint8_t*)&c->regs.eax) + 1;
        case 5: return ((uint8_t*)&c->regs.ecx) + 1;
        case 6: return ((uint8_t*)&c->regs.edx) + 1;
        default: return ((uint8_t*)&c->regs.ebx) + 1;
    }
}

// 8-bit flag updates (the shared 32-bit helpers test bit 31).
static inline void jit_flags8_add(x86_context* c, uint8_t a, uint8_t b, uint8_t r) {
    uint32_t f = c->regs.eflags & ~(X86_CF | X86_ZF | X86_SF | X86_OF | X86_PF | X86_AF);
    if (r == 0) f |= X86_ZF;
    if (r & 0x80) f |= X86_SF;
    if ((int)a + (int)b > 0xFF) f |= X86_CF;
    if (((a ^ r) & (b ^ r)) & 0x80) f |= X86_OF;
    if (!__builtin_parity(r)) f |= X86_PF;
    if ((a ^ b ^ r) & 0x10) f |= X86_AF;
    c->regs.eflags = f;
}

static inline void jit_flags8_sub(x86_context* c, uint8_t a, uint8_t b, uint8_t r) {
    uint32_t f = c->regs.eflags & ~(X86_CF | X86_ZF | X86_SF | X86_OF | X86_PF | X86_AF);
    if (r == 0) f |= X86_ZF;
    if (r & 0x80) f |= X86_SF;
    if ((int)a - (int)b < 0) f |= X86_CF;
    if (((a ^ b) & (a ^ r)) & 0x80) f |= X86_OF;
    if (!__builtin_parity(r)) f |= X86_PF;
    if ((a ^ b ^ r) & 0x10) f |= X86_AF;
    c->regs.eflags = f;
}

// Inline copies of the interpreter's flag updates. They are kept identical to
// x86_update_flags_* in x86_exec.cpp on purpose: the block executor performs
// millions of ALU ops, and a non-inlined call per op was measurable.
static inline void jit_flags_add32(x86_context* c, uint32_t a, uint32_t b, uint32_t r) {
    uint32_t f = c->regs.eflags & ~(X86_CF | X86_ZF | X86_SF | X86_OF | X86_PF | X86_AF);
    if (r == 0) f |= X86_ZF;
    if (r & 0x80000000u) f |= X86_SF;
    if (r < a) f |= X86_CF;
    if (((a ^ r) & (b ^ r)) & 0x80000000u) f |= X86_OF;
    if (!__builtin_parity(r & 0xFF)) f |= X86_PF;
    c->regs.eflags = f;
}

static inline void jit_flags_sub32(x86_context* c, uint32_t a, uint32_t b, uint32_t r) {
    uint32_t f = c->regs.eflags & ~(X86_CF | X86_ZF | X86_SF | X86_OF | X86_PF | X86_AF);
    if (r == 0) f |= X86_ZF;
    if (r & 0x80000000u) f |= X86_SF;
    if (a < b) f |= X86_CF;
    if (((a ^ b) & (a ^ r)) & 0x80000000u) f |= X86_OF;
    if (!__builtin_parity(r & 0xFF)) f |= X86_PF;
    c->regs.eflags = f;
}

static inline void jit_flags_logic32(x86_context* c, uint32_t r) {
    uint32_t f = c->regs.eflags & ~(X86_CF | X86_ZF | X86_SF | X86_OF | X86_PF);
    if (r == 0) f |= X86_ZF;
    if (r & 0x80000000u) f |= X86_SF;
    if (!__builtin_parity(r & 0xFF)) f |= X86_PF;
    c->regs.eflags = f;
}

static inline void jit_alu32(x86_context* c, int sub, uint32_t* d, uint32_t s) {
    uint32_t a = *d, r;
    switch (sub) {
        case 0: r = a + s; jit_flags_add32(c, a, s, r); *d = r; break;
        case 1: r = a | s; jit_flags_logic32(c, r); *d = r; break;
        case 4: r = a & s; jit_flags_logic32(c, r); *d = r; break;
        case 5: r = a - s; jit_flags_sub32(c, a, s, r); *d = r; break;
        case 6: r = a ^ s; jit_flags_logic32(c, r); *d = r; break;
        default: r = a - s; jit_flags_sub32(c, a, s, r); break; // CMP
    }
}

static inline void jit_alu8(x86_context* c, int sub, uint8_t* d, uint8_t s) {
    uint8_t a = *d, r;
    switch (sub) {
        case 0: r = (uint8_t)(a + s); jit_flags8_add(c, a, s, r); *d = r; break;
        case 1: r = (uint8_t)(a | s); jit_flags_logic32(c, r); *d = r; break;
        case 4: r = (uint8_t)(a & s); jit_flags_logic32(c, r); *d = r; break;
        case 5: r = (uint8_t)(a - s); jit_flags8_sub(c, a, s, r); *d = r; break;
        case 6: r = (uint8_t)(a ^ s); jit_flags_logic32(c, r); *d = r; break;
        default: r = (uint8_t)(a - s); jit_flags8_sub(c, a, s, r); break;
    }
}

// ---- Cache --------------------------------------------------------------
static inline uint32_t jit_hash(uint32_t va) {
    return ((va * 2654435761u) >> 22) & (JIT_BUCKETS - 1);
}

static jit_cache* jit_get_cache(x86_context* c) {
    if (!c->jit_cache) c->jit_cache = calloc(1, sizeof(jit_cache));
    return (jit_cache*)c->jit_cache;
}

static jit_block* jit_build(x86_context* c, jit_cache* k, uint32_t va) {
    uint32_t idx = jit_hash(va);
    jit_block* b = k->buckets[idx];
    if (!b) {
        b = (jit_block*)calloc(1, sizeof(jit_block));
        if (!b) return nullptr;
        k->buckets[idx] = b;
    } else if (b->start != JIT_EMPTY) {
        k->evictions++;
    }

    b->start = va;
    b->epoch = c->code_epoch;
    b->nops = 0;

    uint32_t p = va;
    while (b->nops < JIT_MAX_OPS) {
        if (!x86_page_mapped(c, p >> X86_PAGE_SHIFT)) break;
        jit_op op;
        int len = jit_decode(c, p, &op);
        if (len <= 0) break;
        b->ops[b->nops++] = op;
        p += (uint32_t)len;
        uint8_t kind = op.kind;
        if (kind == J_JMP || kind == J_JCC || kind == J_JMP_R || kind == J_CALL_R ||
            kind == J_CALL_REL || kind == J_RET || kind == J_RET_I || kind == J_HLT)
            break;
    }
    b->end = p;

    if (b->nops == 0) {
        b->start = JIT_EMPTY; // never match this slot again without a rebuild
        return nullptr;
    }

    // Record the pages this block occupies so a store there invalidates it.
    for (uint32_t q = va & ~0xFFFu; q < p; q += X86_PAGE_SIZE) {
        uint32_t page = q >> X86_PAGE_SHIFT;
        c->code_pages[page >> 6] |= 1ull << (page & 63);
    }
    k->decoded++;
    return b;
}

int x86_jit_step(x86_context* c) {
    if (!c->jit_enabled || !c->mem.base || !c->running) return 0;
    if (c->insn_count >= c->insn_limit) return 0;

    uint32_t va = c->regs.eip;

    // Import traps are API calls, not code; x86_step owns them.
    if (trt_pe32_is_trap(va)) return 0;

    jit_cache* k = jit_get_cache(c);
    if (!k) return 0;

    uint32_t idx = jit_hash(va);
    jit_block* b = k->buckets[idx];
    if (!b || b->start != va || b->epoch != c->code_epoch) {
        b = jit_build(c, k, va);
        if (!b) return 0;
        k->buckets[idx] = b;
    }

    uint32_t* R[8] = {
        &c->regs.eax, &c->regs.ecx, &c->regs.edx, &c->regs.ebx,
        &c->regs.esp, &c->regs.ebp, &c->regs.esi, &c->regs.edi
    };

    // The limit can only be crossed inside x86_step (one instruction at a
    // time), so a block that would overshoot hands control back for an exact
    // count instead of checking the counter on every op.
    if (b->nops >= c->insn_limit || c->insn_count + b->nops >= c->insn_limit) return 0;
    c->insn_count += b->nops;

    for (uint32_t i = 0; i < b->nops; i++) {
        const jit_op& op = b->ops[i];
        k->insns++;

        // Resolve the memory operand once per executing op.
        uint32_t ea = 0;
        if (op.is_mem) {
            ea = (uint32_t)op.ea_disp;
            if (op.ea_base != JIT_NO_REG) ea += *R[op.ea_base];
            if (op.ea_idx != JIT_NO_REG) ea += *R[op.ea_idx] * op.ea_scale;
            if (op.ea_fs) ea += c->regs.fs_base;
        }

        switch (op.kind) {
        case J_NOP:
            break;

        case J_MOV_RR:
            *R[op.dst] = *R[op.src];
            break;
        case J_MOV_RM:
            *R[op.dst] = x86_mem_read32(c, ea);
            break;
        case J_MOV_MR:
            x86_mem_write32(c, ea, *R[op.src]);
            break;
        case J_MOV_RI:
            if (op.is_mem) x86_mem_write32(c, ea, op.imm);
            else *R[op.dst] = op.imm;
            break;

        case J_ALU_RR:
            jit_alu32(c, op.sub, R[op.dst], *R[op.src]);
            break;
        case J_ALU_RM:
            jit_alu32(c, op.sub, R[op.dst], x86_mem_read32(c, ea));
            break;
        case J_ALU_MR: {
            uint32_t v = x86_mem_read32(c, ea);
            jit_alu32(c, op.sub, &v, *R[op.src]);
            if (op.sub != 7) x86_mem_write32(c, ea, v); // CMP does not store
            break;
        }
        case J_ALU_RI:
            if (op.is_mem) {
                uint32_t v = x86_mem_read32(c, ea);
                jit_alu32(c, op.sub, &v, op.imm);
                if (op.sub != 7) x86_mem_write32(c, ea, v);
            } else {
                jit_alu32(c, op.sub, R[op.dst], op.imm);
            }
            break;

        case J_TEST_RR:
            jit_flags_logic32(c, *R[op.dst] & *R[op.src]);
            break;
        case J_TEST_RM:
            jit_flags_logic32(c, x86_mem_read32(c, ea) & *R[op.dst]);
            break;

        case J_LEA:
            *R[op.dst] = ea;
            break;

        case J_INC: {
            uint32_t cf = c->regs.eflags & X86_CF;
            uint32_t a = *R[op.dst], r = a + 1;
            jit_flags_add32(c, a, 1, r);
            c->regs.eflags = (c->regs.eflags & ~X86_CF) | cf;
            *R[op.dst] = r;
            break;
        }
        case J_DEC: {
            uint32_t cf = c->regs.eflags & X86_CF;
            uint32_t a = *R[op.dst], r = a - 1;
            jit_flags_sub32(c, a, 1, r);
            c->regs.eflags = (c->regs.eflags & ~X86_CF) | cf;
            *R[op.dst] = r;
            break;
        }

        case J_PUSH_R:
            c->regs.esp -= 4;
            x86_mem_write32(c, c->regs.esp, *R[op.dst]);
            break;
        case J_POP_R:
            *R[op.dst] = x86_mem_read32(c, c->regs.esp);
            c->regs.esp += 4;
            break;
        case J_PUSH_I:
            c->regs.esp -= 4;
            x86_mem_write32(c, c->regs.esp, op.imm);
            break;
        case J_PUSH_RM:
            c->regs.esp -= 4;
            x86_mem_write32(c, c->regs.esp, op.is_mem ? x86_mem_read32(c, ea) : *R[op.rm]);
            break;

        case J_MOVZX8:
            *R[op.dst] = op.is_mem ? (uint32_t)x86_mem_read8(c, ea) : (uint32_t)*jit_r8(c, op.rm);
            break;
        case J_MOVZX16:
            *R[op.dst] = op.is_mem ? (uint32_t)x86_mem_read16(c, ea) : (uint32_t)(*R[op.rm] & 0xFFFF);
            break;
        case J_MOVSX8:
            *R[op.dst] = op.is_mem ? (uint32_t)(int32_t)(int8_t)x86_mem_read8(c, ea)
                                   : (uint32_t)(int32_t)(int8_t)*jit_r8(c, op.rm);
            break;
        case J_MOVSX16:
            *R[op.dst] = op.is_mem ? (uint32_t)(int32_t)(int16_t)x86_mem_read16(c, ea)
                                   : (uint32_t)(int32_t)(int16_t)(*R[op.rm] & 0xFFFF);
            break;

        case J_MOV8_RR:
            *jit_r8(c, op.dst) = *jit_r8(c, op.src);
            break;
        case J_MOV8_RM:
            *jit_r8(c, op.dst) = x86_mem_read8(c, ea);
            break;
        case J_MOV8_MR:
            x86_mem_write8(c, ea, *jit_r8(c, op.src));
            break;
        case J_MOV8_RI:
            if (op.is_mem) x86_mem_write8(c, ea, (uint8_t)op.imm);
            else *jit_r8(c, op.dst) = (uint8_t)op.imm;
            break;

        case J_ALU8_RR:
            jit_alu8(c, op.sub, jit_r8(c, op.dst), *jit_r8(c, op.src));
            break;
        case J_ALU8_RM:
            jit_alu8(c, op.sub, jit_r8(c, op.dst), x86_mem_read8(c, ea));
            break;
        case J_ALU8_MR: {
            uint8_t v = x86_mem_read8(c, ea);
            jit_alu8(c, op.sub, &v, *jit_r8(c, op.src));
            if (op.sub != 7) x86_mem_write8(c, ea, v);
            break;
        }

        case J_SETCC: {
            uint8_t v = x86_condition(c->regs.eflags, op.sub) ? 1 : 0;
            if (op.is_mem) x86_mem_write8(c, ea, v);
            else *jit_r8(c, op.dst) = v;
            break;
        }
        case J_CMOVCC:
            if (x86_condition(c->regs.eflags, op.sub))
                *R[op.dst] = op.is_mem ? x86_mem_read32(c, ea) : *R[op.rm];
            break;

        case J_JMP:
            c->regs.eip = op.target;
            return 1;
        case J_JCC:
            c->regs.eip = x86_condition(c->regs.eflags, op.sub) ? op.target : b->end;
            return 1;
        case J_JMP_R:
            c->regs.eip = op.is_mem ? x86_mem_read32(c, ea) : *R[op.rm];
            return 1;
        case J_CALL_R: {
            uint32_t t = op.is_mem ? x86_mem_read32(c, ea) : *R[op.rm];
            c->regs.esp -= 4;
            x86_mem_write32(c, c->regs.esp, b->end);
            c->regs.eip = t;
            return 1;
        }
        case J_CALL_REL:
            c->regs.esp -= 4;
            x86_mem_write32(c, c->regs.esp, b->end);
            c->regs.eip = op.target;
            return 1;
        case J_RET:
            c->regs.eip = x86_mem_read32(c, c->regs.esp);
            c->regs.esp += 4;
            return 1;
        case J_RET_I:
            c->regs.eip = x86_mem_read32(c, c->regs.esp);
            c->regs.esp += 4 + op.imm;
            return 1;

        case J_HLT:
            x86_halt(c, b->end - 1);
            return 1;

        default:
            // Unknown kinds never leave the decoder; treat as a hard stop so a
            // future bug fails loudly instead of silently mis-executing.
            if (c->log) c->log("x86/jit: internal error: unknown op kind");
            c->running = 0;
            c->execution = X86_EXEC_JIT_FAILURE;
            return -1;
        }
    }

    c->regs.eip = b->end;
    return 1;
}

void x86_jit_flush(x86_context* c) {
    c->code_epoch++;
}

void x86_jit_note_write(x86_context* c, uint32_t addr) {
    x86_note_code_write(c, addr);
}

// Stats live in the context's cache; a null cache reports zeros.
void x86_jit_get_stats(x86_context* c, x86_jit_stats* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    jit_cache* k = (jit_cache*)c->jit_cache;
    if (!k) return;
    out->decoded = k->decoded;
    out->evictions = k->evictions;
}
