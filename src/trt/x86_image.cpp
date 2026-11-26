// tiramisu/src/trt/x86_image.cpp
// Bridge between trt_image (PE32 mapping) and the clean-room interpreter.
#include "x86_32.h"
#include "images.h"
#include "pe.h"
#include "thunk.h"
#include "dynamic_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

struct trap_ctx {
  char last[160];
  trt_x86_ctx* c;
};

static trt_resolver g_trap_rs{};

typedef u64 (*host_call4)(u64, u64, u64, u64);

// A normal C call passes the first four integer args in the platform's ABI
// registers (Win64: rcx/rdx/r8/r9; SysV: rdi/rsi/rdx/rcx), so the compiler
// handles the 32->64 widening for us. The 32-bit guest's arguments live on
// its stack: [esp] is the return address, [esp+4..] the arguments.
u64 call_host_bridge(u64 fn, trt_x86_ctx* c) {
  u64 args[4] = {0, 0, 0, 0};
  for (int i = 0; i < 4; ++i) {
    u32 v = 0;
    if (!trt_x86_mem_read(c, c->r[X86_ESP] + 4u + 4u * (u32)i, 4, &v)) {
      args[i] = v;  // zero-extended, like a 32-bit push
    }
  }
  return ((host_call4)fn)(args[0], args[1], args[2], args[3]);
}

static const u32 kReturnSentinel = 0xfffffff0u;  // clean RET from a guest proc

int image_trap(trt_x86_ctx* c, u32 va, void* user) {
  if (va == kReturnSentinel) {
    c->stop_code = 0x100;
    return 0x100;  // end the run; the guest returned from its function
  }
  trap_ctx* t = (trap_ctx*)user;
  if (trt_pe32_is_trap(va)) {
    int idx = (int)((va - TRT_PE32_TRAP_BASE) >> 2);
    char mod[64] = "?", sym[96] = "?";
    u16 ord = 0;
    u32 trap_va = 0;
    trt_pe32_trap_info(idx, mod, sizeof(mod), sym, sizeof(sym), &ord,
                       &trap_va);
    snprintf(t->last, sizeof(t->last), "%s!%s", mod, sym);
    char line[256];
    snprintf(line, sizeof(line), "x86-32: trap %s!%s", mod, sym);
    trt_core_log(line);

    // Resolve the SAME function the 64-bit loader would use, then call it
    // with the guest's 32-bit stack arguments widened to 64 bits.
    void* fn = g_trap_rs.resolve
                  ? g_trap_rs.resolve(g_trap_rs.user, mod, sym, ord)
                  : trt_resolve_module(nullptr, mod, sym, ord);
    if (!fn) {
      fn = trt_dynamic_synthesize_api(mod, sym, ord);
    }
    u32 ret_va = 0;
    trt_x86_mem_read(c, c->r[X86_ESP], 4, &ret_va);
    if (!fn) {
      snprintf(t->last, sizeof(t->last), "%s!%s (unresolved -> 0)", mod, sym);
      c->r[X86_EAX] = 0;
    } else {
      u64 r = call_host_bridge((u64)fn, c);
      c->r[X86_EAX] = (u32)r;
      snprintf(t->last, sizeof(t->last), "%s!%s -> %u", mod, sym,
               (unsigned)(u32)r);
    }
    c->r[X86_ESP] += 4;
    c->eip = ret_va;  // the guest's own return address, never a guess
    return 0;
  }
  snprintf(t->last, sizeof(t->last), "out-of-window 0x%08x", va);
  char line[224];
  snprintf(line, sizeof(line), "x86-32: NAMED fault at 0x%08x", va);
  trt_core_log(line);
  return 0x80000003u;
}

}  // namespace

void trt_x86_set_resolver(const trt_resolver* rs) {
  if (rs) g_trap_rs = *rs;
}

// layout 0 = standard cdecl (return address, then args)
// layout 1 = caller-EBP chain (saved EBP, args, then return address) — used by
//            CRT thunks that inherit the caller's EBP across a tail jump.
int trt_x86_run_cdecl_ex(trt_image* img, u32 entry_va, const u32 args[3],
                         u64 max_instructions, u32* out_eax, int layout) {
  if (!img || !img->is_pe32 || !img->base) return 2;
  const u32 isz = (u32)img->size_of_image;
  if (!isz || isz > 64u * 1024u * 1024u) return 2;
  const u32 stack_size = 0x10000;
  const u32 teb_size = 0x2000;  // FS-segment TEB page for 32-bit guests
  const size_t total = (size_t)isz + stack_size + teb_size;
  u8* mem = (u8*)calloc(total, 1);
  if (!mem) return 2;
  memcpy(mem, img->base, isz);

  trt_x86_ctx c{};
  c.mem = mem; c.mem_va = img->guest_base; c.mem_size = (u32)total;
  c.eip = entry_va;
  // 32-bit Windows process model: FS -> TEB with a real SEH chain, stack
  // bounds, self pointer and stack guard. CRT thunks walk FS:[0] and read
  // TEB stack fields; a zeroed page makes them return through garbage.
  c.fs_base = img->guest_base + isz + stack_size + 0x1000;
  {
    const u32 stack_top = img->guest_base + isz + stack_size;
    const u32 stack_bot = img->guest_base + isz;
    u32 self = c.fs_base;
    u32 guard = 0x0abbccddu;
    u32 endchain = 0xffffffffu;
    // EXCEPTION_REGISTRATION_RECORD just below the TEB.
    const u32 seh_rec = c.fs_base - 16;
    u32 handler = 0xfffffff0u;  // sentinel: a guest SEH dispatch is named
    trt_x86_mem_write(&c, seh_rec, 4, &endchain);
    trt_x86_mem_write(&c, seh_rec + 4, 4, &handler);
    trt_x86_mem_write(&c, c.fs_base + 0x00, 4, &seh_rec);   // ExceptionList
    trt_x86_mem_write(&c, c.fs_base + 0x04, 4, &stack_top); // StackBase
    trt_x86_mem_write(&c, c.fs_base + 0x08, 4, &stack_bot); // StackLimit
    trt_x86_mem_write(&c, c.fs_base + 0x18, 4, &self);      // Self
    trt_x86_mem_write(&c, c.fs_base + 0x28, 4, &guard);     // guard cookie
  }
  const u32 sp = img->guest_base + isz + stack_size - 64;
  c.r[X86_ESP] = sp;
  c.r[X86_EBP] = sp;
  if (layout == 0) {
    trt_x86_mem_write(&c, sp, 4, &kReturnSentinel);        // return address
    for (int i = 0; i < 3; ++i) {
      u32 a = args ? args[i] : 0;
      trt_x86_mem_write(&c, sp + 4u + 4u * (u32)i, 4, &a);
    }
  } else {
    u32 caller_ebp = sp + 16;                              // inherited frame
    trt_x86_mem_write(&c, sp, 4, &caller_ebp);
    for (int i = 0; i < 3; ++i) {
      u32 a = args ? args[i] : 0;
      trt_x86_mem_write(&c, sp + 4u + 4u * (u32)i, 4, &a);
    }
    trt_x86_mem_write(&c, sp + 16, 4, &kReturnSentinel);
  }
  trap_ctx t{}; t.c = &c;
  c.trap = &image_trap; c.trap_user = &t;

  int rc = trt_x86_run(&c, max_instructions);
  memcpy(img->base, mem, isz);
  if (out_eax) *out_eax = c.r[X86_EAX];
  free(mem);
  return rc;
}

int trt_x86_run_cdecl(trt_image* img, u32 entry_va, const u32 args[3],
                      u64 max_instructions, u32* out_eax) {
  return trt_x86_run_cdecl_ex(img, entry_va, args, max_instructions,
                              out_eax, 0);
}

// Auto-adapt: try the standard frame, then the inherited-EBP frame. A guest
// whose CRT thunk pops a caller EBP completes on the second layout instead of
// returning through garbage — the engine fits the ABI to the title.
int trt_x86_run_entry_auto(trt_image* img, u32 entry_va, const u32 args[3],
                           u64 max_instructions, u32* out_eax, int* layout_out) {
  for (int layout = 0; layout < 2; ++layout) {
    u32 eax = 0;
    int rc = trt_x86_run_cdecl_ex(img, entry_va, args, max_instructions,
                                  &eax, layout);
    if (rc == 0 || rc == 1) {
      if (out_eax) *out_eax = eax;
      if (layout_out) *layout_out = layout;
      return rc;
    }
  }
  if (layout_out) *layout_out = -1;
  return 2;
}

int trt_x86_run_image(trt_image* img, u32 entry_va, u32 stack_size,
                      u64 max_instructions, u32* out_eax,
                      char* trap_out, size_t trap_cap) {
  if (!img || !img->is_pe32 || !img->base || stack_size < 4096) return 2;
  const u32 isz = (u32)img->size_of_image;
  if (!isz || isz > 64u * 1024u * 1024u) return 2;

  // One flat guest window: [guest_base, guest_base + image + stack).
  const size_t total = (size_t)isz + stack_size;
  u8* mem = (u8*)calloc(total, 1);
  if (!mem) return 2;
  memcpy(mem, img->base, isz);
  const u32 stack_va = img->guest_base + isz;

  trt_x86_ctx c{};
  c.mem = mem;
  c.mem_va = img->guest_base;
  c.mem_size = (u32)total;
  c.eip = entry_va;
  c.r[X86_ESP] = stack_va + stack_size - 4;
  c.r[X86_EBP] = c.r[X86_ESP];

  trap_ctx t{};
  t.c = &c;
  c.trap = &image_trap;
  c.trap_user = &t;

  int rc = trt_x86_run(&c, max_instructions);

  // Copy back data writes (the stack region is scratch; image pages are not).
  memcpy(img->base, mem, isz);
  if (out_eax) *out_eax = c.r[X86_EAX];
  if (trap_out && trap_cap) {
    snprintf(trap_out, trap_cap, "%s", t.last);
  }
  free(mem);
  return rc;
}
