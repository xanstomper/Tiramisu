// tests/test_x86_exec.cpp
// Executes real x86 instruction bytes through x86_step() and checks the
// architectural result: CPUID, RDTSC, SSE2 packed-double, MMX/SSE integer
// SIMD, 8-bit ALU, REP MOVSB, PUSHA/POPA, FXSAVE, and the import dispatch
// path (TLS + GetLastError special imports).
//
// This is the execution-level gate for the 32-bit interpreter: it proves the
// new opcodes actually compute, not merely compile.
#include "../src/trt/x86_cpu.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>

extern "C" uint32_t x86_dispatch_import(x86_context* ctx, int trap_index,
                                       const char* module, const char* symbol,
                                       int* out_nargs, int* out_cdecl);
extern "C" int x86_is_cdecl(const char* module, const char* symbol);
extern "C" int x86_arg_count(const char* module, const char* symbol);
extern "C" int trt_pe32_is_trap(uint32_t guest_va);
extern "C" uint32_t trt_pe32_bind_dynamic(const char* module, const char* symbol,
                                          uint32_t ordinal, void* host_fn);
extern "C" void x86_set_hotloop_limit(uint64_t limit);

static void put_cstr(x86_context* ctx, uint32_t addr, const char* s) {
    do { x86_mem_write8(ctx, addr++, (uint8_t)*s); } while (*s++);
}

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS %s\n", msg); } \
    else { printf("  FAIL %s\n", msg); ++g_fail; } \
} while (0)

static void put(x86_context* ctx, uint32_t addr, const uint8_t* bytes, size_t n) {
    x86_mem_commit(ctx, addr, n < 4096 ? 4096 : n);
    memcpy(ctx->mem.base + addr, bytes, n);
}

// Load a guest program at 0x10000 and run it to HLT with the recompiler on
// (`jit` = 1) or off (pure interpreter).
static void run_prog(x86_context* ctx, const uint8_t* code, size_t n, int jit, uint64_t limit) {
    memset(ctx, 0, sizeof(*ctx));
    x86_cpu_init(ctx);
    x86_mem_commit(ctx, 0x00010000, 0x10000);
    x86_mem_commit(ctx, 0x00100000, 0x10000);
    memcpy(ctx->mem.base + 0x10000, code, n);
    ctx->regs.esp = 0x0010F000;
    ctx->regs.ebp = ctx->regs.esp;
    ctx->regs.eip = 0x10000;
    ctx->jit_enabled = jit;
    ctx->insn_limit = limit;
    x86_run(ctx);
}

static void setup(x86_context* ctx) {
    x86_cpu_init(ctx);
    x86_mem_commit(ctx, 0x00010000, 0x1000); // code
    x86_mem_commit(ctx, 0x00100000, 0x10000); // stack
    ctx->regs.esp = 0x0010F000;
    ctx->regs.ebp = ctx->regs.esp;
    ctx->running = 1;
    ctx->insn_limit = 10000;
}

int main() {
    x86_context ctx;

    printf("== CPUID ==\n");
    setup(&ctx);
    {
        const uint8_t code[] = {0x0F, 0xA2}; // cpuid
        put(&ctx, 0x10000, code, sizeof(code));
        ctx.regs.eip = 0x10000;
        ctx.regs.eax = 0;
        int rc = x86_step(&ctx);
        CHECK(rc == 0, "cpuid executes");
        CHECK(ctx.regs.ebx == 0x756E6547u, "vendor 'Genu' in EBX");
        CHECK(ctx.regs.edx == 0x49656E69u, "vendor 'ineI' in EDX");
        CHECK(ctx.regs.ecx == 0x6C65746Eu, "vendor 'ntel' in ECX");
    }

    printf("== CPUID leaf 1 (features) ==\n");
    {
        const uint8_t code[] = {0x0F, 0xA2};
        put(&ctx, 0x10000, code, sizeof(code));
        ctx.regs.eip = 0x10000;
        ctx.regs.eax = 1;
        x86_step(&ctx);
        CHECK((ctx.regs.edx & (1u << 25)) != 0, "SSE feature bit");
        CHECK((ctx.regs.edx & (1u << 26)) != 0, "SSE2 feature bit");
        CHECK((ctx.regs.edx & (1u << 23)) != 0, "MMX feature bit");
    }

    printf("== XCHG [mem], reg (0x87 with a memory operand) ==\n");
    {
        x86_mem_write32(&ctx, 0x10400, 0x11223344u);
        ctx.regs.ecx = 0x10400;
        ctx.regs.eax = 0xAABBCCDDu;
        const uint8_t code[] = {0x87, 0x01}; // xchg dword [ecx], eax
        put(&ctx, 0x10000, code, sizeof(code));
        ctx.regs.eip = 0x10000;
        int rc = x86_step(&ctx);
        CHECK(rc == 0, "xchg [mem],reg executes");
        CHECK(ctx.regs.eax == 0x11223344u,
              "register receives the old memory value");
        CHECK(x86_mem_read32(&ctx, 0x10400) == 0xAABBCCDDu,
              "memory receives the register value");
        CHECK(ctx.regs.eip == 0x10002,
              "EIP advances past the whole instruction");
    }

    printf("== RDTSC ==\n");
    {
        const uint8_t code[] = {0x0F, 0x31};
        put(&ctx, 0x10000, code, sizeof(code));
        ctx.regs.eip = 0x10000;
        x86_step(&ctx);
        uint64_t t1 = ((uint64_t)ctx.regs.edx << 32) | ctx.regs.eax;
        ctx.regs.eip = 0x10000;
        x86_step(&ctx);
        uint64_t t2 = ((uint64_t)ctx.regs.edx << 32) | ctx.regs.eax;
        CHECK(t2 > t1, "RDTSC advances");
    }

    printf("== SSE2 ADDPD (66 0F 58 C1) ==\n");
    {
        double a[2] = {1.0, 2.0}, b[2] = {3.0, 4.0}, r[2] = {0, 0};
        memcpy(ctx.regs.xmm[0], a, 16);
        memcpy(ctx.regs.xmm[1], b, 16);
        const uint8_t code[] = {0x66, 0x0F, 0x58, 0xC1}; // addpd xmm0, xmm1
        put(&ctx, 0x10000, code, sizeof(code));
        ctx.regs.eip = 0x10000;
        x86_step(&ctx);
        memcpy(r, ctx.regs.xmm[0], 16);
        CHECK(r[0] == 4.0 && r[1] == 6.0, "packed double sum");
    }

    printf("== SSE2 MULSD (F2 0F 59 C1) ==\n");
    {
        double a[2] = {2.5, 0.0}, b[2] = {4.0, 0.0}, r[2] = {0, 0};
        memcpy(ctx.regs.xmm[0], a, 16);
        memcpy(ctx.regs.xmm[1], b, 16);
        const uint8_t code[] = {0xF2, 0x0F, 0x59, 0xC1}; // mulsd xmm0, xmm1
        put(&ctx, 0x10000, code, sizeof(code));
        ctx.regs.eip = 0x10000;
        x86_step(&ctx);
        memcpy(r, ctx.regs.xmm[0], 16);
        CHECK(r[0] == 10.0, "scalar double product");
        CHECK(r[1] == 0.0, "upper lane untouched");
    }

    printf("== SSE2 PADDD (66 0F FE C1) ==\n");
    {
        uint32_t a[4] = {1, 2, 3, 4}, b[4] = {10, 20, 30, 40}, r[4] = {0, 0, 0, 0};
        memcpy(ctx.regs.xmm[0], a, 16);
        memcpy(ctx.regs.xmm[1], b, 16);
        const uint8_t code[] = {0x66, 0x0F, 0xFE, 0xC1}; // paddd xmm0, xmm1
        put(&ctx, 0x10000, code, sizeof(code));
        ctx.regs.eip = 0x10000;
        x86_step(&ctx);
        memcpy(r, ctx.regs.xmm[0], 16);
        CHECK(r[0] == 11 && r[1] == 22 && r[2] == 33 && r[3] == 44, "packed dword add");
    }

    printf("== SSE2 PXOR (66 0F EF C1) ==\n");
    {
        uint32_t a[4] = {0xFFFF0000u, 0, 0, 0}, b[4] = {0x0000FFFFu, 0, 0, 0};
        memcpy(ctx.regs.xmm[0], a, 16);
        memcpy(ctx.regs.xmm[1], b, 16);
        const uint8_t code[] = {0x66, 0x0F, 0xEF, 0xC1};
        put(&ctx, 0x10000, code, sizeof(code));
        ctx.regs.eip = 0x10000;
        x86_step(&ctx);
        uint32_t r[4]; memcpy(r, ctx.regs.xmm[0], 16);
        CHECK(r[0] == 0xFFFFFFFFu, "pxor clears differing bits");
    }

    printf("== MMX PADDB (0F FC C1) ==\n");
    {
        ctx.regs.mm[0] = 0x04030201ull;
        ctx.regs.mm[1] = 0x01010101ull;
        const uint8_t code[] = {0x0F, 0xFC, 0xC1}; // paddb mm0, mm1
        put(&ctx, 0x10000, code, sizeof(code));
        ctx.regs.eip = 0x10000;
        x86_step(&ctx);
        CHECK(ctx.regs.mm[0] == 0x05040302ull, "mmx byte-wise add");
    }

    printf("== 8-bit ALU: ADD AL, CL (00 C8) ==\n");
    {
        const uint8_t code[] = {0x00, 0xC8};
        put(&ctx, 0x10000, code, sizeof(code));
        ctx.regs.eip = 0x10000;
        ctx.regs.eax = 0x00000010;
        ctx.regs.ecx = 0x00000005;
        x86_step(&ctx);
        CHECK((ctx.regs.eax & 0xFF) == 0x15, "AL = 0x15");
        CHECK((ctx.regs.eax & 0xFFFFFF00u) == 0, "upper EAX untouched");
    }

    printf("== 0x80 group: CMP AL, imm8 (80 F8 20) ==\n");
    {
        const uint8_t code[] = {0x80, 0xF8, 0x20}; // cmp al, 0x20
        put(&ctx, 0x10000, code, sizeof(code));
        ctx.regs.eip = 0x10000;
        ctx.regs.eax = 0x00000020;
        x86_step(&ctx);
        CHECK((ctx.regs.eflags & X86_ZF) != 0, "ZF set on equal CMP");
        CHECK((ctx.regs.eax & 0xFF) == 0x20, "CMP leaves AL intact");
    }

    printf("== REP MOVSB (F3 A4) ==\n");
    {
        const uint8_t code[] = {0xF3, 0xA4};
        put(&ctx, 0x10000, code, sizeof(code));
        x86_mem_commit(&ctx, 0x00020000, 0x1000);
        x86_mem_commit(&ctx, 0x00030000, 0x1000);
        const char* src = "tiramisu32";
        memcpy(ctx.mem.base + 0x20000, src, strlen(src) + 1);
        memset(ctx.mem.base + 0x30000, 0, 16);
        ctx.regs.eip = 0x10000;
        ctx.regs.esi = 0x20000;
        ctx.regs.edi = 0x30000;
        ctx.regs.ecx = (uint32_t)strlen(src) + 1;
        ctx.regs.eflags &= ~X86_DF;
        x86_step(&ctx);
        CHECK(memcmp(ctx.mem.base + 0x30000, src, strlen(src) + 1) == 0, "REP MOVSB copied bytes");
        CHECK(ctx.regs.ecx == 0, "ECX decremented to zero");
    }

    printf("== PUSHA / POPA (60 / 61) ==\n");
    {
        const uint8_t code[] = {0x60, 0x61};
        put(&ctx, 0x10000, code, sizeof(code));
        ctx.regs.eip = 0x10000;
        ctx.regs.esp = 0x0010F000;
        uint32_t save[8] = {ctx.regs.eax, ctx.regs.ecx, ctx.regs.edx, ctx.regs.ebx,
                            ctx.regs.esp, ctx.regs.ebp, ctx.regs.esi, ctx.regs.edi};
        x86_step(&ctx);
        CHECK(ctx.regs.esp == 0x0010F000 - 32, "PUSHA pushes 8 dwords");
        x86_step(&ctx);
        CHECK(ctx.regs.esp == 0x0010F000, "POPA restores ESP");
        CHECK(ctx.regs.eax == save[0] && ctx.regs.ecx == save[1] &&
              ctx.regs.edx == save[2] && ctx.regs.ebx == save[3],
              "POPA restores registers");
    }

    printf("== FXSAVE / FXRSTOR round-trip ==\n");
    {
        x86_mem_commit(&ctx, 0x00040000, 0x1000);
        double d[2] = {3.25, -7.5};
        memcpy(ctx.regs.xmm[3], d, 16);
        ctx.regs.fpu_cw = 0x027F;
        const uint8_t save_code[] = {0x0F, 0xAE, 0x05, 0x00, 0x00, 0x04, 0x00}; // fxsave [0x40000]
        put(&ctx, 0x10000, save_code, sizeof(save_code));
        ctx.regs.eip = 0x10000;
        x86_step(&ctx);
        memset(ctx.regs.xmm[3], 0, 16);
        ctx.regs.fpu_cw = 0;
        const uint8_t load_code[] = {0x0F, 0xAE, 0x0D, 0x00, 0x00, 0x04, 0x00}; // fxrstor [0x40000]
        put(&ctx, 0x10000, load_code, sizeof(load_code));
        ctx.regs.eip = 0x10000;
        x86_step(&ctx);
        double r[2]; memcpy(r, ctx.regs.xmm[3], 16);
        CHECK(r[0] == 3.25 && r[1] == -7.5, "FXSAVE/FXRSTOR restores XMM");
        CHECK(ctx.regs.fpu_cw == 0x027F, "FXSAVE/FXRSTOR restores control word");
    }

    printf("== contiguous guest memory: VA is a host pointer ==\n");
    {
        x86_mem_commit(&ctx, 0x00050000, 0x1000);
        uint32_t v = 0xDEADBEEF;
        x86_mem_write32(&ctx, 0x00050000 + 8, v);
        uint32_t* hp = (uint32_t*)(void*)(ctx.mem.base + 0x00050000 + 8);
        CHECK(*hp == v, "host pointer sees guest write");
        *hp = 0x12345678;
        CHECK(x86_mem_read32(&ctx, 0x00050000 + 8) == 0x12345678, "guest read sees host write");
    }

    printf("== uncommitted page reads 0, never crashes ==\n");
    {
        uint8_t b = x86_mem_read8(&ctx, 0x0F000000);
        CHECK(b == 0, "unmapped read returns 0");
    }

    printf("== import dispatch: GetLastError / SetLastError ==\n");
    {
        ctx.last_error = 0;
        // Guest stack: [esp] = return address, then one arg.
        ctx.regs.esp = 0x0010E000;
        x86_mem_write32(&ctx, ctx.regs.esp, 0x12345678);     // return address
        x86_mem_write32(&ctx, ctx.regs.esp + 4, 0x2A);       // arg0
        int nargs = -1, cdecl_call = -1;
        uint32_t r = x86_dispatch_import(&ctx, -1, "kernel32", "SetLastError", &nargs, &cdecl_call);
        CHECK(r == 0 && ctx.last_error == 0x2A, "SetLastError stores per-thread code");
        CHECK(nargs == 1 && cdecl_call == 0, "SetLastError is stdcall with 1 arg");
        ctx.regs.esp = 0x0010E000;
        x86_mem_write32(&ctx, ctx.regs.esp, 0x12345678);
        r = x86_dispatch_import(&ctx, -1, "kernel32", "GetLastError", &nargs, &cdecl_call);
        CHECK(r == 0x2A, "GetLastError returns the stored code");
        CHECK(nargs == 0, "GetLastError has 0 args");
    }

    printf("== import dispatch: TLS alloc/set/get ==\n");
    {
        ctx.regs.esp = 0x0010E000;
        x86_mem_write32(&ctx, ctx.regs.esp, 0x11111111);
        int nargs = 0, cdecl_call = 0;
        uint32_t slot = x86_dispatch_import(&ctx, -1, "kernel32", "TlsAlloc", &nargs, &cdecl_call);
        CHECK(slot < 64, "TlsAlloc returns a valid slot");

        x86_mem_write32(&ctx, ctx.regs.esp + 4, slot);
        x86_mem_write32(&ctx, ctx.regs.esp + 8, 0xCAFEBABE);
        uint32_t ok = x86_dispatch_import(&ctx, -1, "kernel32", "TlsSetValue", &nargs, &cdecl_call);
        CHECK(ok == 1, "TlsSetValue succeeds");
        CHECK(nargs == 2, "TlsSetValue takes 2 args");

        x86_mem_write32(&ctx, ctx.regs.esp, 0x11111111);
        x86_mem_write32(&ctx, ctx.regs.esp + 4, slot);
        uint32_t got = x86_dispatch_import(&ctx, -1, "kernel32", "TlsGetValue", &nargs, &cdecl_call);
        CHECK(got == 0xCAFEBABE, "TlsGetValue returns the stored value");
    }

    printf("== import dispatch: InterlockedIncrement ==\n");
    {
        x86_mem_commit(&ctx, 0x00060000, 0x1000);
        x86_mem_write32(&ctx, 0x00060000, 41);
        ctx.regs.esp = 0x0010E000;
        x86_mem_write32(&ctx, ctx.regs.esp, 0x11111111);
        x86_mem_write32(&ctx, ctx.regs.esp + 4, 0x00060000);
        int nargs = 0, cdecl_call = 0;
        uint32_t r = x86_dispatch_import(&ctx, -1, "kernel32", "InterlockedIncrement", &nargs, &cdecl_call);
        CHECK(r == 42, "returns incremented value");
        CHECK(x86_mem_read32(&ctx, 0x00060000) == 42, "memory updated");
    }

    printf("== import dispatch: VirtualQuery / VirtualProtect ==\n");
    {
        x86_mem_commit(&ctx, 0x00061000, 0x1000);
        ctx.regs.esp = 0x0010E000;
        x86_mem_write32(&ctx, ctx.regs.esp, 0xDEADBEEF);
        x86_mem_write32(&ctx, ctx.regs.esp + 4, 0x00061000);
        x86_mem_write32(&ctx, ctx.regs.esp + 8, 0x00062000);
        x86_mem_write32(&ctx, ctx.regs.esp + 12, 28);
        int nargs = 0, cdecl_call = 0;
        uint32_t r = x86_dispatch_import(&ctx, -1, "kernel32", "VirtualQuery",
                                         &nargs, &cdecl_call);
        CHECK(r == 28, "VirtualQuery returns 28 (sizeof MBI32)");
        CHECK(nargs == 3, "VirtualQuery takes 3 args");
        CHECK(x86_mem_read32(&ctx, 0x00062000) == 0x00061000u,
              "MBI.BaseAddress is page-aligned");
        CHECK(x86_mem_read32(&ctx, 0x00062000 + 16) == 0x1000u,
              "MBI.State == MEM_COMMIT");

        ctx.regs.esp = 0x0010E000;
        x86_mem_write32(&ctx, ctx.regs.esp, 0xDEADBEEF);
        x86_mem_write32(&ctx, ctx.regs.esp + 4, 0x00061000);
        x86_mem_write32(&ctx, ctx.regs.esp + 8, 0x1000);
        x86_mem_write32(&ctx, ctx.regs.esp + 12, 0x40);
        x86_mem_write32(&ctx, ctx.regs.esp + 16, 0x00062100);
        nargs = 0; cdecl_call = 0;
        uint32_t old_prot = 0;
        r = x86_dispatch_import(&ctx, -1, "kernel32", "VirtualProtect",
                                &nargs, &cdecl_call);
        CHECK(r == 1, "VirtualProtect reports success");
        CHECK(nargs == 4, "VirtualProtect takes 4 args");
        CHECK(x86_mem_read32(&ctx, 0x00062100) != 0,
              "VirtualProtect writes the old protect value");
    }

    printf("== import dispatch: GetSystemInfo / IsThreadAFiber ==\n");
    {
        x86_mem_commit(&ctx, 0x00063000, 0x1000);
        ctx.regs.esp = 0x0010E000;
        x86_mem_write32(&ctx, ctx.regs.esp, 0xDEADBEEF);
        x86_mem_write32(&ctx, ctx.regs.esp + 4, 0x00063000); // LPSYSTEM_INFO
        int nargs = 0, cdecl_call = 0;
        uint32_t r = x86_dispatch_import(&ctx, -1, "kernel32", "GetSystemInfo",
                                         &nargs, &cdecl_call);
        CHECK(r == 0, "GetSystemInfo returns void (0)");
        CHECK(nargs == 1, "GetSystemInfo takes 1 arg");
        CHECK(x86_mem_read32(&ctx, 0x00063000 + 4) == 4096, "dwPageSize == 4096");
        CHECK(x86_mem_read32(&ctx, 0x00063000 + 20) == 8, "dwNumberOfProcessors == 8");

        trt_modules_register_defaults();
        trt_bridge_register_kernel32();
        ctx.regs.esp = 0x0010E000;
        x86_mem_write32(&ctx, ctx.regs.esp, 0xDEADBEEF);
        nargs = -1; cdecl_call = -1;
        r = x86_dispatch_import(&ctx, -1, "kernel32", "IsThreadAFiber",
                                &nargs, &cdecl_call);
        CHECK(nargs == 0, "IsThreadAFiber takes 0 args");
    }

    printf("== 32-bit runtime loader: LoadLibrary -> GetProcAddress -> CALL ==\n");
    {
        // The host registers the bridge modules at boot; a bare interpreter
        // test has to do the same before runtime symbol resolution can find a
        // bridge function to trap.
        trt_modules_register_defaults();
        trt_bridge_register_kernel32();
        x86_mem_commit(&ctx, 0x00070000, 0x2000);
        put_cstr(&ctx, 0x00070000, "kernel32.dll");
        put_cstr(&ctx, 0x00070080, "GetTickCount");
        put_cstr(&ctx, 0x00070100, "NoSuchKernelSymbol");
        int nargs = -1, cdecl_call = -1;

        // LoadLibraryA("kernel32.dll") -> a handle a 32-bit register can hold.
        ctx.regs.esp = 0x0010E000;
        x86_mem_write32(&ctx, ctx.regs.esp, 0xDEADBEEF);
        x86_mem_write32(&ctx, ctx.regs.esp + 4, 0x00070000);
        uint32_t h = x86_dispatch_import(&ctx, -1, "kernel32", "LoadLibraryA",
                                         &nargs, &cdecl_call);
        CHECK(h != 0 && h < 0x00100000u,
              "LoadLibraryA returns a small 32-bit module handle");
        CHECK(nargs == 1, "LoadLibraryA takes 1 arg");

        // GetProcAddress(handle, "GetTickCount") -> trap, NEVER a host pointer.
        ctx.regs.esp = 0x0010E000;
        x86_mem_write32(&ctx, ctx.regs.esp, 0xDEADBEEF);
        x86_mem_write32(&ctx, ctx.regs.esp + 4, h);
        x86_mem_write32(&ctx, ctx.regs.esp + 8, 0x00070080);
        uint32_t va = x86_dispatch_import(&ctx, -1, "kernel32", "GetProcAddress",
                                          &nargs, &cdecl_call);
        CHECK(nargs == 2, "GetProcAddress takes 2 args");
        CHECK(va >= 0xF0000000u, "GetProcAddress answers in the trap range");
        CHECK(trt_pe32_is_trap(va), "the returned VA is a registered trap");

        ctx.regs.esp = 0x0010E000;
        x86_mem_write32(&ctx, ctx.regs.esp, 0xDEADBEEF);
        x86_mem_write32(&ctx, ctx.regs.esp + 4, h);
        x86_mem_write32(&ctx, ctx.regs.esp + 8, 0x00070080);
        uint32_t va2 = x86_dispatch_import(&ctx, -1, "kernel32", "GetProcAddress",
                                           &nargs, &cdecl_call);
        CHECK(va2 == va, "repeat lookups share one trap (deduped)");

        // A guest CALL to that trap must reach the 64-bit bridge and come back.
        x86_mem_commit(&ctx, 0x00080000, 0x1000);
        x86_mem_write8(&ctx, 0x00080000, 0xF4);  // HLT
        ctx.running = 1;
        ctx.regs.esp = 0x0010E000;
        x86_mem_write32(&ctx, ctx.regs.esp, 0x00080000);  // return address
        ctx.regs.eax = 0;
        ctx.regs.eip = va;
        int rc = x86_step(&ctx);
        CHECK(rc == 0, "calling a trap executes the bridge, not an instruction");
        CHECK(ctx.regs.eip == 0x00080000u,
              "trap dispatch returns to the guest's return address");
        CHECK(ctx.regs.esp == 0x0010E004u,
              "stdcall cleanup popped exactly the return address (0 args)");
        CHECK(ctx.regs.eax != 0, "GetTickCount returned a live tick count");

        // An unresolved symbol answers 0 and NAMES the gap.
        ctx.regs.esp = 0x0010E000;
        x86_mem_write32(&ctx, ctx.regs.esp, 0xDEADBEEF);
        x86_mem_write32(&ctx, ctx.regs.esp + 4, h);
        x86_mem_write32(&ctx, ctx.regs.esp + 8, 0x00070100);
        uint32_t miss = x86_dispatch_import(&ctx, -1, "kernel32", "GetProcAddress",
                                            &nargs, &cdecl_call);
        CHECK(miss == 0, "an unresolved symbol returns 0 (no fake pointer)");
    }

    printf("== guest arena allocator (32-bit heap) ==\n");
    {
        x86_heap_stats st;
        x86_guest_heap_stats(&st);
        CHECK(st.arena_base == 0x20000000u && st.arena_end == 0xC0000000u,
              "arena is [0x20000000, 0xC0000000) inside the 4 GB reservation");

        uint32_t p = x86_guest_alloc(&ctx, 100, 0);
        CHECK(p >= st.arena_base && p < st.arena_end,
              "alloc returns an arena VA (survives a 32-bit register)");
        CHECK((p & 7u) == 0, "allocation is 8-byte aligned");
        x86_mem_write32(&ctx, p, 0x12345678);
        CHECK(x86_mem_read32(&ctx, p) == 0x12345678,
              "arena memory is readable and writable");
        CHECK(x86_guest_blocksize(&ctx, p) == 104,
              "blocksize reports the 8-byte-aligned size");

        uint32_t q = x86_guest_alloc(&ctx, 8, 1);
        CHECK(q != 0 && q != p && x86_mem_read32(&ctx, q) == 0,
              "zero-fill flag clears the payload");
        CHECK(x86_guest_free(&ctx, p) == 1, "free of a live block succeeds");
        CHECK(x86_guest_free(&ctx, p) == 0, "double free is refused");
        CHECK(x86_guest_free(&ctx, 0x00400000u) == 0,
              "free of a non-arena pointer is refused");

        uint32_t again = x86_guest_alloc(&ctx, 64, 0);
        CHECK(again == p, "the freed block is reused (first fit)");
        uint32_t grown = x86_guest_realloc(&ctx, again, 512, 0);
        CHECK(grown != 0 && grown != again, "realloc moves a block that is too small");
        CHECK(x86_guest_blocksize(&ctx, grown) >= 512, "realloc grew the block");
        x86_guest_free(&ctx, grown);
        x86_guest_free(&ctx, q);

        // ...and the API layer routes to it.
        int nargs = 0, cdecl_call = 0;
        auto call1 = [&](const char* mod, const char* sym, uint32_t a0, uint32_t a1,
                         uint32_t a2, uint32_t a3) -> uint32_t {
            ctx.regs.esp = 0x0010E000;
            x86_mem_write32(&ctx, ctx.regs.esp, 0xDEADBEEF);
            x86_mem_write32(&ctx, ctx.regs.esp + 4, a0);
            x86_mem_write32(&ctx, ctx.regs.esp + 8, a1);
            x86_mem_write32(&ctx, ctx.regs.esp + 12, a2);
            x86_mem_write32(&ctx, ctx.regs.esp + 16, a3);
            return x86_dispatch_import(&ctx, -1, mod, sym, &nargs, &cdecl_call);
        };

        uint32_t hb = call1("kernel32", "HeapAlloc", 1u /*heap*/, 0x8u /*ZERO*/, 32u, 0u);
        CHECK(nargs == 3, "HeapAlloc takes 3 args");
        CHECK(hb >= st.arena_base && hb < st.arena_end,
              "HeapAlloc returns an arena VA");
        CHECK(x86_mem_read32(&ctx, hb) == 0, "HEAP_ZERO_MEMORY honours the flag");
        x86_mem_write32(&ctx, hb, 7);
        CHECK(call1("kernel32", "HeapFree", 1u, 0u, hb, 0u) == 1,
              "HeapFree frees the block");

        uint32_t va_mem = call1("kernel32", "VirtualAlloc", 0u, 8192u, 0x3000u, 4u);
        CHECK(va_mem >= st.arena_base && va_mem < st.arena_end,
              "VirtualAlloc pulls from the arena too");

        uint32_t mb = call1("msvcrt", "malloc", 128u, 0u, 0u, 0u);
        CHECK(mb >= st.arena_base && mb < st.arena_end,
              "msvcrt malloc is arena-hosted");
        CHECK(call1("msvcrt", "free", mb, 0u, 0u, 0u) == 0,
              "msvcrt free returns void");
        CHECK(x86_guest_blocksize(&ctx, mb) == 0,
              "the freed malloc block is no longer live");
    }

    printf("== calling-convention metadata ==\n");
    {
        CHECK(x86_is_cdecl("msvcrt", "printf") == 1, "printf classified __cdecl");
        CHECK(x86_is_cdecl("kernel32", "Sleep") == 0, "Sleep classified __stdcall");
        CHECK(x86_arg_count("kernel32", "CreateFileW") == 7, "CreateFileW arg count 7");
        CHECK(x86_arg_count("user32", "CreateWindowExW") == 12, "CreateWindowExW arg count 12");
        CHECK(x86_arg_count("d3d11", "D3D11CreateDevice") == 7, "D3D11CreateDevice arg count 7");
        CHECK(x86_arg_count("kernel32", "TlsSetValue") == 2, "TlsSetValue arg count 2");
    }

    printf("== ModRM [ebp-4] store/load (89 45 FC / 8B 45 FC) ==\n");
    {
        const uint8_t store[] = {0x89, 0x45, 0xFC}; // mov [ebp-4], eax
        put(&ctx, 0x10000, store, sizeof(store));
        ctx.regs.eip = 0x10000;
        ctx.regs.ebp = 0x0010E000;
        ctx.regs.eax = 0xA1B2C3D4u;
        x86_step(&ctx);
        CHECK(x86_mem_read32(&ctx, 0x0010E000 - 4) == 0xA1B2C3D4u, "disp8 store hits [ebp-4]");
        const uint8_t load[] = {0x8B, 0x45, 0xFC}; // mov eax, [ebp-4]
        put(&ctx, 0x10000, load, sizeof(load));
        ctx.regs.eip = 0x10000;
        ctx.regs.eax = 0;
        x86_step(&ctx);
        CHECK(ctx.regs.eax == 0xA1B2C3D4u, "disp8 load reads [ebp-4]");
    }

    printf("== SIB addressing [esp+ecx*4+8] ==\n");
    {
        const uint8_t code[] = {0x8B, 0x44, 0x8C, 0x08}; // mov eax,[esp+ecx*4+8]
        put(&ctx, 0x10000, code, sizeof(code));
        ctx.regs.eip = 0x10000;
        ctx.regs.esp = 0x0010D000;
        ctx.regs.ecx = 3;
        x86_mem_commit(&ctx, 0x0010D000 + 3 * 4 + 8, 0x1000);
        x86_mem_write32(&ctx, 0x0010D000 + 3 * 4 + 8, 0x5EEDF00Du);
        x86_step(&ctx);
        CHECK(ctx.regs.eax == 0x5EEDF00Du, "SIB base+index*scale+disp");
    }

    printf("== absolute disp32 [0x00050000+16] ==\n");
    {
        const uint8_t code[] = {0x8B, 0x05, 0x10, 0x00, 0x05, 0x00}; // mov eax,[0x50010]
        put(&ctx, 0x10000, code, sizeof(code));
        ctx.regs.eip = 0x10000;
        x86_mem_write32(&ctx, 0x00050010, 0x0BADF00Du);
        x86_step(&ctx);
        CHECK(ctx.regs.eax == 0x0BADF00Du, "disp32-only operand");
        CHECK(ctx.regs.eip == 0x10006, "EIP advanced past disp32");
    }

    printf("== SHUFPS (0F C6 C1 1B) ==\n");
    {
        float a[4] = {1, 2, 3, 4}, b[4] = {5, 6, 7, 8}, r[4] = {0, 0, 0, 0};
        memcpy(ctx.regs.xmm[0], a, 16);
        memcpy(ctx.regs.xmm[1], b, 16);
        const uint8_t code[] = {0x0F, 0xC6, 0xC1, 0x1B}; // shufps xmm0,xmm1,0x1B
        put(&ctx, 0x10000, code, sizeof(code));
        ctx.regs.eip = 0x10000;
        x86_step(&ctx);
        memcpy(r, ctx.regs.xmm[0], 16);
        // imm 0x1B selects lanes {3,2} from dst and {1,0} from src
        CHECK(r[0] == 4 && r[1] == 3 && r[2] == 6 && r[3] == 5, "shufps lane selection");
    }

    printf("== RET imm16 (C2 08 00) ==\n");
    {
        const uint8_t code[] = {0xC2, 0x08, 0x00}; // ret 8
        put(&ctx, 0x10000, code, sizeof(code));
        ctx.regs.eip = 0x10000;
        ctx.regs.esp = 0x0010D000;
        x86_mem_write32(&ctx, ctx.regs.esp, 0x00123456);
        x86_step(&ctx);
        CHECK(ctx.regs.eip == 0x00123456, "RET pops return address");
        CHECK(ctx.regs.esp == 0x0010D000 + 4 + 8, "RET imm16 cleans args");
    }

    printf("== multi-byte NOP (0F 1F 44 00 00) ==\n");
    {
        const uint8_t code[] = {0x0F, 0x1F, 0x44, 0x00, 0x00};
        put(&ctx, 0x10000, code, sizeof(code));
        ctx.regs.eip = 0x10000;
        int rc = x86_step(&ctx);
        CHECK(rc == 0 && ctx.regs.eip == 0x10005, "5-byte alignment NOP consumed");
    }

    x86_mem_free(&ctx);

    // ==================================================================
    // Basic-block recompiler
    // ==================================================================
    printf("== JIT: loop executes to completion ==\n");
    {
        // xor eax,eax; mov ecx,1000; L: add eax,ecx; dec ecx; jnz L; hlt
        static const uint8_t loop_prog[] = {
            0x31, 0xC0,
            0xB9, 0xE8, 0x03, 0x00, 0x00,
            0x01, 0xC8,
            0x49,
            0x75, 0xFB,
            0xF4
        };
        x86_context a = {};
        run_prog(&a, loop_prog, sizeof(loop_prog), 0, 5000000);
        uint32_t interp_eax = a.regs.eax, interp_ecx = a.regs.ecx;
        x86_mem_free(&a);

        x86_context b = {};
        run_prog(&b, loop_prog, sizeof(loop_prog), 1, 5000000);
        CHECK(b.regs.eax == 500500, "sum 1..1000 = 500500");
        CHECK(b.regs.ecx == 0, "loop counter exhausted");
        CHECK(b.regs.eax == interp_eax && b.regs.ecx == interp_ecx,
              "JIT matches interpreter on the loop");

        x86_jit_stats st;
        x86_jit_get_stats(&b, &st);
        CHECK(st.decoded > 0, "recompiler built at least one block");
        x86_mem_free(&b);
    }

    printf("== x86: unsigned CRT startup loop ==\n");
    {
        // The MSVC startup loop increments an offset by sizeof(CRITICAL_SECTION).
        // JB must fall through when CMP finds the offset equals the upper bound.
        const uint8_t bytes[] = {0x31, 0xF6, 0x83, 0xC6, 0x18, 0x83, 0xFE, 0x18,
                                 0x72, 0xF8, 0xF4};
        for (int jit = 0; jit <= 1; ++jit) {
            x86_context c = {};
            run_prog(&c, bytes, sizeof(bytes), jit, 64);
            CHECK(c.execution == X86_EXEC_OK && c.regs.esi == 24,
                  "JB falls through on equal operands");
            x86_mem_free(&c);
        }
    }

    printf("== x86: positive and negated condition codes ==\n");
    {
        // Independent reference masks for the final EFLAGS of selected CMPs.
        const struct { uint32_t flags; uint16_t mask; } cases[] = {
            {0, 0xAAAA},                      // O, B, E, BE, S, P, L, LE all false
            {X86_CF | X86_PF | X86_SF, 0x5566},
            {X86_PF | X86_ZF, 0x665A},
            {X86_OF | X86_SF, 0xA9A9},
        };
        for (const auto& example : cases) {
            x86_context c = {};
            setup(&c);
            for (unsigned code = 0; code < 16; ++code) {
                x86_mem_write8(&c, 0x10000, 0x0F);
                x86_mem_write8(&c, 0x10001, static_cast<uint8_t>(0x90 + code));
                x86_mem_write8(&c, 0x10002, 0xC0);  // SETcc AL
                c.regs.eip = 0x10000;
                c.regs.eflags = example.flags;
                CHECK(x86_step(&c) == 0 && (c.regs.eax & 0xFF) == ((example.mask >> code) & 1),
                      "SETcc matches the x86 flags");
            }
            x86_mem_free(&c);
        }
    }

    printf("== x86: indirect JMP preserves the caller's return address ==\n");
    {
        // A common import thunk is FF /4 [IAT]. Its target's RET must
        // return directly to the original caller.
        const uint8_t bytes[] = {
            0xC7, 0x05, 0x00, 0x10, 0x01, 0x00,  // mov [0x11000],target
            0x1C, 0x00, 0x01, 0x00,
            0xE8, 0x06, 0x00, 0x00, 0x00,        // call thunk
            0x83, 0xC0, 0x01, 0xF4,             // add eax,1; hlt
            0x90, 0x90,
            0xFF, 0x25, 0x00, 0x10, 0x01, 0x00,  // thunk: jmp [0x11000]
            0xCC,
            0xB8, 0x29, 0x00, 0x00, 0x00, 0xC3   // target: mov eax,41; ret
        };
        for (int jit = 0; jit <= 1; ++jit) {
            x86_context c = {};
            run_prog(&c, bytes, sizeof(bytes), jit, 64);
            CHECK(c.execution == X86_EXEC_OK && c.regs.eax == 42 &&
                      c.regs.esp == 0x0010F000,
                  "indirect tail jump returns to the original caller");
            x86_mem_free(&c);
        }
    }

    printf("== x86: signed register IMUL and overflow ==\n");
    {
        x86_context c = {};
        setup(&c);
        const uint8_t imul[] = {0x0F, 0xAF, 0xC1};  // imul eax,ecx
        memcpy(c.mem.base + 0x10000, imul, sizeof(imul));
        c.regs.eip = 0x10000;
        c.regs.eax = uint32_t(-16);
        c.regs.ecx = 3;
        CHECK(x86_step(&c) == 0 && c.regs.eax == uint32_t(-48) &&
                  !(c.regs.eflags & (X86_CF | X86_OF)),
              "signed IMUL computes and clears overflow for a fitting product");
        c.regs.eip = 0x10000;
        c.regs.eax = 0x7FFFFFFF;
        CHECK(x86_step(&c) == 0 && c.regs.eax == 0x7FFFFFFD &&
                  (c.regs.eflags & (X86_CF | X86_OF)) == (X86_CF | X86_OF),
              "signed IMUL truncates and sets carry and overflow");
        CHECK(x86_arg_count("kernel32.dll", "FlsGetValue2") == 1,
              "FlsGetValue2 pops just the index");
        x86_mem_free(&c);
    }

    printf("== x86: GetStartupInfo uses the guest structure size ==\n");
    {
        x86_context c = {};
        setup(&c);
        constexpr uint32_t info = 0x00100000;
        x86_mem_write32(&c, c.regs.esp + 4, info);
        const char* const functions[] = {"GetStartupInfoA", "GetStartupInfoW"};
        for (const char* name : functions) {
            x86_mem_write32(&c, info + 64, 0xFFFFFFFF);
            x86_mem_write32(&c, info + 68, 0x11223344);
            int nargs = 0, is_cdecl = 0;
            x86_dispatch_import(&c, -1, "kernel32.dll", name, &nargs, &is_cdecl);
            CHECK(nargs == 1 && x86_mem_read32(&c, info) == 68 &&
                      x86_mem_read32(&c, info + 64) == 0 &&
                      x86_mem_read32(&c, info + 68) == 0x11223344,
                  "startup info initializes 68 guest bytes and preserves the stack");
        }
        x86_mem_free(&c);
    }

    printf("== x86: rotate through CL and shift through memory ==\n");
    {
        const uint8_t bytes[] = {
            0xB8, 0x01, 0x00, 0x00, 0x80,             // mov eax,0x80000001
            0xB9, 0x01, 0x00, 0x00, 0x00,             // mov ecx,1
            0xD3, 0xC8,                               // ror eax,cl
            0x0F, 0x92, 0xC2,                         // setc dl
            0xA3, 0x04, 0x10, 0x01, 0x00,             // mov [0x11004],eax
            0xBB, 0x00, 0x10, 0x01, 0x00,             // mov ebx,0x11000
            0xC1, 0x63, 0x04, 0x01,                   // shl [ebx+4],1
            0xF4
        };
        x86_context c = {};
        run_prog(&c, bytes, sizeof(bytes), 0, 64);
        CHECK(c.execution == X86_EXEC_OK && c.regs.eax == 0xC0000000u &&
                  c.regs.edx == 1 && x86_mem_read32(&c, 0x11004) == 0x80000000u,
              "rotate sets carry and memory shift writes back to the right address");
        x86_mem_free(&c);
    }

    printf("== x86: command line stays in guest memory ==\n");
    {
        x86_context c = {};
        setup(&c);
        trt_bridge_set_exe_path("Z:\\Fixture.exe");
        trt_bridge_set_cmd_args("--example");
        int nargs = 0, is_cdecl = 0;
        const auto ansi = x86_dispatch_import(&c, -1, "kernel32.dll", "GetCommandLineA",
                                              &nargs, &is_cdecl);
        const auto wide = x86_dispatch_import(&c, -1, "kernel32.dll", "GetCommandLineW",
                                              &nargs, &is_cdecl);
        const auto again = x86_dispatch_import(&c, -1, "kernel32.dll", "GetCommandLineA",
                                               &nargs, &is_cdecl);
        const char expected[] = "\"Z:\\Fixture.exe\" --example";
        bool matches = ansi && wide && ansi != wide && ansi == again;
        for (size_t i = 0; matches && i < sizeof(expected); ++i)
            matches = x86_mem_read8(&c, ansi + static_cast<uint32_t>(i)) == expected[i] &&
                      x86_mem_read16(&c, wide + static_cast<uint32_t>(2 * i)) == expected[i];
        CHECK(matches, "ANSI and UTF-16 calls return stable guest addresses and correct text");
        trt_bridge_set_cmd_args("");
        x86_mem_free(&c);
    }

    printf("== x86: bit tests with immediate and signed memory indices ==\n");
    {
        x86_context c = {};
        setup(&c);
        const uint8_t bytes[] = {
            0x0F, 0xBA, 0x25, 0x04, 0x00, 0x10, 0x00, 0x21, // bt [0x100004],33
            0x0F, 0xBB, 0x48, 0x04,                         // btc [eax+4],ecx
        };
        memcpy(c.mem.base + 0x10000, bytes, sizeof(bytes));
        x86_mem_write32(&c, 0x100000, 0x80000000u);
        x86_mem_write32(&c, 0x100004, 2);
        c.regs.eip = 0x10000;
        c.regs.eax = 0x100000;
        c.regs.ecx = uint32_t(-1);
        CHECK(x86_step(&c) == 0 && (c.regs.eflags & X86_CF),
              "immediate bit index wraps inside the addressed dword");
        CHECK(x86_step(&c) == 0 && (c.regs.eflags & X86_CF) &&
                  x86_mem_read32(&c, 0x100000) == 0 && x86_mem_read32(&c, 0x100004) == 2,
              "negative bit index updates the preceding dword");
        x86_mem_free(&c);
    }

    printf("== x86: register ADC and SBB use the incoming carry ==\n");
    {
        const uint8_t bytes[] = {
            0xF9,                                     // stc
            0x1B, 0xC0,                               // sbb eax,eax
            0x0F, 0x92, 0xC2,                         // setc dl
            0x13, 0xC8,                               // adc ecx,eax
            0xF4
        };
        x86_context c = {};
        run_prog(&c, bytes, sizeof(bytes), 0, 64);
        CHECK(c.execution == X86_EXEC_OK && c.regs.eax == 0xFFFFFFFFu &&
                  c.regs.ecx == 0 && c.regs.edx == 1 &&
                  (c.regs.eflags & (X86_CF | X86_ZF)) == (X86_CF | X86_ZF),
              "borrow survives SBB and rolls ADC over with carry");
        x86_mem_free(&c);
    }

    printf("== x86: INC/DEC byte keeps the instruction stream aligned ==\n");
    {
        const uint8_t bytes[] = {
            0xB9, 0x00, 0x10, 0x01, 0x00,             // mov ecx,0x11000
            0xB8, 0xFF, 0x80, 0x00, 0x00,             // mov eax,0x80ff
            0x88, 0x41, 0x5C,                         // mov [ecx+0x5c],al
            0xF9,                                     // stc
            0xFE, 0x81, 0x5C, 0x00, 0x00, 0x00,       // inc byte [ecx+0x5c]
            0x0F, 0x94, 0xC2,                         // setz dl
            0xFE, 0xCC,                               // dec ah
            0xF4
        };
        x86_context c = {};
        run_prog(&c, bytes, sizeof(bytes), 0, 64);
        CHECK(c.execution == X86_EXEC_OK && c.regs.eax == 0x7FFF &&
                  c.regs.edx == 1 && x86_mem_read8(&c, 0x1105C) == 0 &&
                  (c.regs.eflags & (X86_CF | X86_OF)) == (X86_CF | X86_OF),
              "memory INC and high-byte DEC update operands and preserve carry");
        x86_mem_free(&c);
    }

    printf("== x86: MOV immediate honors the encoded destination width ==\n");
    {
        const uint8_t bytes[] = {
            0xC7, 0x05, 0x00, 0x10, 0x01, 0x00,  // mov dword [0x11000],0x44332211
            0x11, 0x22, 0x33, 0x44,
            0x66, 0xC7, 0x05, 0x00, 0x10, 0x01, 0x00, 0xAA, 0xBB, // mov word [0x11000],0xbbaa
            0xB8, 0x00, 0x00, 0xFE, 0xCA,       // mov eax,0xcafe0000
            0x66, 0xB8, 0x78, 0x56,             // mov ax,0x5678
            0xC6, 0xC4, 0x21,                   // mov ah,0x21
            0xF4
        };
        x86_context c = {};
        run_prog(&c, bytes, sizeof(bytes), 0, 64);
        CHECK(c.execution == X86_EXEC_OK && c.regs.eax == 0xCAFE2178 &&
                  x86_mem_read32(&c, 0x11000) == 0x4433BBAA,
              "word immediates consume two bytes and byte immediates honor AH");
        x86_mem_free(&c);
    }

    printf("== x86: guest exit is separate from a privileged instruction ==\n");
    {
        uint8_t normal[] = {0x31, 0xC0, 0xC3}; // xor eax,eax; ret
        uint8_t bad[] = {0xF4};                // HLT in guest code
        for (int jit = 0; jit <= 1; ++jit) {
            x86_context c = {};
            x86_cpu_init(&c);
            c.jit_enabled = jit;
            CHECK(x86_run_pe32(&c, normal, sizeof(normal),
                                0x400000, 0, 0x100000, 0x10000) == X86_EXEC_OK,
                  "a PE32 entry can return to the host sentinel");
            CHECK(x86_run_pe32(&c, bad, sizeof(bad),
                                0x400000, 0, 0x100000, 0x10000) == X86_EXEC_BAD_INSTRUCTION,
                  "guest HLT is an execution fault");
            x86_mem_free(&c);
        }
        x86_context c = {};
        setup(&c);
        x86_mem_write32(&c, c.regs.esp + 4, 5);
        int nargs = 0, is_cdecl = 0;
        CHECK(x86_dispatch_import(&c, -1, "kernel32.dll", "ExitProcess", &nargs, &is_cdecl) == 5 &&
                  !c.running && c.execution == X86_EXEC_GUEST_ERROR,
              "ExitProcess stops just the guest and retains its error");
        x86_mem_free(&c);
    }

    printf("== x86: PE32 static TLS and process-attach callbacks ==\n");
    {
        uint8_t bytes[64]{};
        const uint8_t entry[] = {
            0x64, 0xA1, 0x2C, 0, 0, 0, 0x8B, 0x00, // eax = FS:[2c][0]
            0x8B, 0x40, 0x04, 0xC3                 // return TLS dword at +4
        };
        const uint8_t attach[] = {
            0x64, 0xA1, 0x2C, 0, 0, 0, 0x8B, 0x00,
            0x8B, 0x4C, 0x24, 0x08, // ecx = reason argument
            0x02, 0x08,             // add cl,[eax], first TLS template byte
            0x88, 0x48, 0x04,       // store the sum in the zero-fill tail
            0xC2, 0x0C, 0x00
        };
        memcpy(bytes, entry, sizeof(entry));
        memcpy(bytes + 32, attach, sizeof(attach));
        bytes[20] = 41;
        memset(bytes + 16, 0xFF, 4); // loader has to write the TLS index
        trt_image image{};
        image.tls_data_rva = 20;
        image.tls_data_size = 4;
        image.tls_zero_fill = 4;
        image.tls_index_rva = 16;
        image.tls_callback_count = 1;
        image.tls_callback_rvas[0] = 32;
        x86_context c = {};
        x86_cpu_init(&c);
        CHECK(x86_run_pe32(&c, bytes, sizeof(bytes), 0x400000, 0, 0x100000, 0x10000, &image) ==
                  X86_EXEC_OK && c.regs.eax == 42 &&
                  x86_mem_read32(&c, 0x400010) == 0,
              "TLS data, zero fill, index and process attach are ready before entry");
        x86_mem_free(&c);
    }

    printf("== x86: unsupported two-byte opcodes fail the run ==\n");
    {
        const uint8_t bytes[] = {0x0F, 0x0B};  // UD2
        x86_context c = {};
        run_prog(&c, bytes, sizeof(bytes), 0, 64);
        CHECK(c.execution == X86_EXEC_BAD_INSTRUCTION,
              "decoded two-byte fault cannot report successful execution");
        x86_mem_free(&c);
    }

    printf("== x86: immediate follows memory displacement ==\n");
    {
        const uint8_t bytes[] = {
            0xBB, 0x00, 0x10, 0x01, 0x00,             // mov ebx,0x11000
            0x83, 0x43, 0x04, 0x13,                   // add dword [ebx+4],19
            0x81, 0xAB, 0x04, 0x00, 0x00, 0x00,       // sub dword [ebx+4],8
            0x08, 0x00, 0x00, 0x00,
            0x8B, 0x43, 0x04,                         // mov eax,[ebx+4]
            0xF4
        };
        for (int jit = 0; jit <= 1; ++jit) {
            x86_context c = {};
            run_prog(&c, bytes, sizeof(bytes), jit, 64);
            CHECK(c.execution == X86_EXEC_OK && c.regs.eax == 11 &&
                      x86_mem_read32(&c, 0x11004) == 11,
                  "both immediate widths use the displacement before the operand value");
            x86_mem_free(&c);
        }
    }

    printf("== JIT: memory operands, CMP/JE, value round-trip ==\n");
    {
        static const uint8_t prog[] = {
            0xB8, 0x34, 0x12, 0x00, 0x00,             // mov eax,0x1234
            0x89, 0x05, 0x00, 0x10, 0x01, 0x00,       // mov [0x11000],eax
            0x81, 0x3D, 0x00, 0x10, 0x01, 0x00,       // cmp dword [0x11000],imm32
            0x34, 0x12, 0x00, 0x00,
            0x74, 0x01,                               // je +1
            0xF4,                                     // hlt (not taken)
            0xB8, 0x01, 0x00, 0x00, 0x00,             // mov eax,1
            0xBB, 0x02, 0x00, 0x00, 0x00,             // mov ebx,2
            0xF4
        };
        x86_context c = {};
        run_prog(&c, prog, sizeof(prog), 1, 1000);
        CHECK(x86_mem_read32(&c, 0x11000) == 0x1234, "store through EA landed");
        CHECK(c.regs.eax == 1, "conditional branch recognised equal operands");
        CHECK(c.regs.ebx == 2, "fall-through path skipped the hlt");
        x86_mem_free(&c);
    }

    printf("== JIT: differential vs interpreter (MOVZX/SETcc/CMOVcc) ==\n");
    {
        static const uint8_t prog[] = {
            0xB8, 0x2A, 0x00, 0x00, 0x00,       // mov eax,42
            0x83, 0xC0, 0x08,                   // add eax,8
            0x0F, 0xB6, 0xD8,                   // movzx ebx,al
            0x83, 0xF8, 0x32,                   // cmp eax,50
            0x0F, 0x94, 0xC1,                   // sete cl
            0x0F, 0x45, 0xD9,                   // cmovne ebx,ecx
            0x89, 0x1D, 0x00, 0x10, 0x01, 0x00, // mov [0x11000],ebx
            0xF4
        };
        x86_context a = {};
        run_prog(&a, prog, sizeof(prog), 0, 1000);
        x86_context b = {};
        run_prog(&b, prog, sizeof(prog), 1, 1000);
        CHECK(a.regs.eax == 50 && a.regs.ebx == 50 && a.regs.ecx == 1,
              "interpreter baseline values");
        CHECK(b.regs.eax == a.regs.eax && b.regs.ebx == a.regs.ebx &&
              b.regs.ecx == a.regs.ecx, "JIT register file matches interpreter");
        CHECK((b.regs.eflags & 0x8D5) == (a.regs.eflags & 0x8D5),
              "JIT flags match interpreter");
        CHECK(x86_mem_read32(&b, 0x11000) == x86_mem_read32(&a, 0x11000),
              "JIT memory effects match interpreter");
        x86_mem_free(&a);
        x86_mem_free(&b);
    }

    printf("== JIT: self-modifying code invalidates blocks ==\n");
    {
        static const uint8_t prog[] = {
            0xB8, 0x05, 0x00, 0x00, 0x00, // mov eax,5
            0xF4
        };
        x86_context c = {};
        run_prog(&c, prog, sizeof(prog), 1, 1000);
        CHECK(c.regs.eax == 5, "first run used the original immediate");

        // Patch the immediate in the guest's own code page.
        x86_mem_write8(&c, 0x10001, 0x07);
        c.running = 1;
        c.regs.eip = 0x10000;
        c.insn_count = 0;
        x86_run(&c);
        CHECK(c.regs.eax == 7, "recompiled block rebuilt after the patch");
        x86_mem_free(&c);
    }

    printf("== JIT: throughput on 1,000,000 iterations ==\n");
    {
        // xor eax,eax; mov ecx,1000000; L: add eax,ecx; dec ecx; jnz L; hlt
        static const uint8_t big_loop[] = {
            0x31, 0xC0,
            0xB9, 0x40, 0x42, 0x0F, 0x00,
            0x01, 0xC8,
            0x49,
            0x75, 0xFB,
            0xF4
        };
        struct timespec t0, t1, t2;
        x86_context a = {};
        clock_gettime(CLOCK_MONOTONIC, &t0);
        run_prog(&a, big_loop, sizeof(big_loop), 0, 20000000);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        x86_context b = {};
        run_prog(&b, big_loop, sizeof(big_loop), 1, 20000000);
        clock_gettime(CLOCK_MONOTONIC, &t2);
        double interp_ms = ((t1.tv_sec - t0.tv_sec) * 1e3) + ((t1.tv_nsec - t0.tv_nsec) / 1e6);
        double jit_ms = ((t2.tv_sec - t1.tv_sec) * 1e3) + ((t2.tv_nsec - t1.tv_nsec) / 1e6);
        printf("  interp %.1f ms (%llu insns)  jit %.1f ms (%llu insns)  speedup %.2fx\n",
               interp_ms, (unsigned long long)a.insn_count,
               jit_ms, (unsigned long long)b.insn_count,
               interp_ms > 0 ? interp_ms / (jit_ms > 0 ? jit_ms : 1e-9) : 0.0);
        CHECK(a.regs.eax == b.regs.eax, "both engines computed the same sum");
        CHECK(jit_ms < interp_ms, "recompiler is faster than the interpreter");
        x86_mem_free(&a);
        x86_mem_free(&b);
    }

    // ==================================================================
    // Hot-loop probe: a spinning guest names its own window
    // ==================================================================
    printf("== HOT LOOP: `jmp $` is reported by name, not silently capped ==\n");
    {
        static char hot_log[4096];
        hot_log[0] = 0;
        x86_set_hotloop_limit(20000);
        x86_context c = {};
        x86_cpu_init(&c);
        c.log = [](const char* s) {
            const size_t used = strlen(hot_log);
            if (used + 2 < sizeof(hot_log))
                snprintf(hot_log + used, sizeof(hot_log) - used, "%s\n", s);
        };
        x86_mem_commit(&c, 0x10000, 0x1000);
        x86_mem_commit(&c, 0x100000, 0x10000);
        const uint8_t spin[] = {0xEB, 0xFE};  // jmp $ (2-byte self jump)
        memcpy(c.mem.base + 0x10000, spin, sizeof(spin));
        c.regs.esp = 0x0010F000;
        c.regs.ebp = c.regs.esp;
        c.regs.eip = 0x10000;
        c.jit_enabled = 0;
        c.insn_limit = 300000;
        CHECK(x86_run(&c) == X86_EXEC_INSTRUCTION_LIMIT,
              "a bounded spin reports exhaustion");
        CHECK(strstr(hot_log, "HOT LOOP") != NULL,
              "stalled window logged by name");
        CHECK(strstr(hot_log, "0x00010000..0x00010000") != NULL,
              "reported window is the spin address");
        CHECK(c.regs.eip == 0x10000, "probe never moves the guest");
        x86_mem_free(&c);
        x86_set_hotloop_limit(20000000);
    }

    // ==================================================================
    // EIP profile: the instruction budget's top regions are named
    // ==================================================================
    printf("== EIP PROFILE: a spinning region is named after a long run ==\n");
    {
        static char prof_log[4096];
        prof_log[0] = 0;
        x86_set_hotloop_limit(0);  // profile only, no hot-loop line
        x86_context c = {};
        x86_cpu_init(&c);
        c.log = [](const char* s) {
            const size_t used = strlen(prof_log);
            if (used + 2 < sizeof(prof_log))
                snprintf(prof_log + used, sizeof(prof_log) - used, "%s\n", s);
        };
        x86_mem_commit(&c, 0x10000, 0x1000);
        x86_mem_commit(&c, 0x100000, 0x10000);
        const uint8_t spin[] = {0xEB, 0xFE};  // jmp $
        memcpy(c.mem.base + 0x10000, spin, sizeof(spin));
        c.regs.esp = 0x0010F000;
        c.regs.ebp = c.regs.esp;
        c.regs.eip = 0x10000;
        c.jit_enabled = 0;
        c.insn_limit = 1200000;
        x86_run(&c);
        CHECK(strstr(prof_log, "eip profile") != NULL,
              "instruction budget is profiled");
        CHECK(strstr(prof_log, "0x00010000..0x0001FFFF") != NULL,
              "hottest region named (0x00010000)");
        CHECK(strstr(prof_log, "hot instructions:") != NULL &&
                  strstr(prof_log, "0x00010000") != NULL,
              "hottest individual instruction named");
        x86_mem_free(&c);
        x86_set_hotloop_limit(20000000);
    }

    // ==================================================================
    // SEH dispatch
    // ==================================================================
    printf("== SEH: handler that searches leaves the exception unhandled ==\n");
    {
        x86_context c = {};
        x86_cpu_init(&c);
        x86_mem_commit(&c, 0x10000, 0x10000);
        x86_mem_commit(&c, 0x100000, 0x10000);
        c.regs.esp = 0x0010F000;
        c.regs.eip = 0x10000;
        c.running = 1;
        c.insn_limit = 100000;

        // long __cdecl handler(void*, void*, CONTEXT*, void*) { return 1; }
        static const uint8_t search_handler[] = { 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 };
        memcpy(c.mem.base + 0x10100, search_handler, sizeof(search_handler));
        x86_mem_write32(&c, 0x10300, 0xFFFFFFFF); // prev
        x86_mem_write32(&c, 0x10304, 0x00010100); // handler
        x86_mem_write32(&c, 0x7FFDE000, 0x00010300); // TEB ExceptionList

        uint32_t info = 0x10000;
        int r = x86_raise_seh(&c, 0x80000003u, 0, 0x10000, 1, &info);
        CHECK(r == -1, "ContinueSearch exhausts the chain");
        CHECK(c.regs.eip == 0x10000, "EIP untouched when nothing handled it");
        x86_mem_free(&c);
    }

    printf("== SEH: handler resumes at CONTEXT.Eip ==\n");
    {
        x86_context c = {};
        x86_cpu_init(&c);
        x86_mem_commit(&c, 0x10000, 0x10000);
        x86_mem_commit(&c, 0x100000, 0x10000);
        c.regs.esp = 0x0010F000;
        c.running = 1;
        c.insn_limit = 100000;

        // return 0; CONTEXT->Eip = 0x10200;
        static const uint8_t resume_handler[] = {
            0xB8, 0x00, 0x00, 0x00, 0x00,             // mov eax,0
            0x8B, 0x4C, 0x24, 0x0C,                   // mov ecx,[esp+0xC]
            0xC7, 0x81, 0xBC, 0x00, 0x00, 0x00, 0x00, 0x02, 0x01, 0x00, // mov dword [ecx+0xBC],0x10200
            0xC3
        };
        memcpy(c.mem.base + 0x10100, resume_handler, sizeof(resume_handler));
        x86_mem_write32(&c, 0x10300, 0xFFFFFFFF);
        x86_mem_write32(&c, 0x10304, 0x00010100);
        x86_mem_write32(&c, 0x7FFDE000, 0x00010300);

        uint32_t info = 0x10000;
        int r = x86_raise_seh(&c, 0x80000003u, 0, 0x10000, 1, &info);
        CHECK(r == 0, "ContinueExecution accepted");
        CHECK(c.regs.eip == 0x10200, "EIP restored from the CONTEXT");
        x86_mem_free(&c);
    }

    printf("== SEH: INT3 delivered through x86_step ==\n");
    {
        x86_context c = {};
        x86_cpu_init(&c);
        x86_mem_commit(&c, 0x10000, 0x10000);
        x86_mem_commit(&c, 0x100000, 0x10000);
        c.regs.esp = 0x0010F000;
        c.regs.eip = 0x10000;
        c.running = 1;
        c.insn_limit = 100000;

        c.mem.base[0x10000] = 0xCC; // int3
        static const uint8_t resume_handler[] = {
            0xB8, 0x00, 0x00, 0x00, 0x00,
            0x8B, 0x4C, 0x24, 0x0C,
            0xC7, 0x81, 0xBC, 0x00, 0x00, 0x00, 0x00, 0x02, 0x01, 0x00,
            0xC3
        };
        memcpy(c.mem.base + 0x10100, resume_handler, sizeof(resume_handler));
        x86_mem_write32(&c, 0x10300, 0xFFFFFFFF);
        x86_mem_write32(&c, 0x10304, 0x00010100);
        x86_mem_write32(&c, 0x7FFDE000, 0x00010300);

        int rc = x86_step(&c);
        CHECK(rc == 0, "INT3 did not halt the guest");
        CHECK(c.regs.eip == 0x10200, "INT3 resumed at the handler's chosen EIP");
        CHECK(c.running == 1, "guest still running after handled INT3");
        x86_mem_free(&c);
    }

    printf("\nRESULT: %s (%d failures)\n", g_fail ? "FAIL" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
