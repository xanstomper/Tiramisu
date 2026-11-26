// tiramisu/tests/test_d3d9.cpp
// The D3D9 bridge: a guest goes through the full D3D9 lifecycle (device,
// texture, vertex/index buffers, locks, state, transform, draw, present)
// and the host hooks observe every GPU action.
#include "../src/trt/trt.h"
#include "../src/trt/bridge_d3d9.h"
#include "../src/trt/bridge_d3d9_meta.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

extern "C" {
int trt_bridge_register_kernel32(void);
int trt_bridge_register_user32(void);
int trt_bridge_register_d3d9(void);
u32 trt_d3d9_last_draw(void);
}

namespace {

struct extent { void* base; size_t len; };
extent g_extents[16];
int g_extent_count = 0;

void* linux_alloc(size_t bytes, u64 preferred_base) {
  void* p = nullptr;
  if (preferred_base) {
    // ASan's shadow gap swallows the guest's usual preferred bases
    // (0x140000000 is INSIDE it — measured "unknown-crash" false positives):
    // under sanitizers, let the kernel pick and let the loader relocate.
#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && \
    (defined(__clang__) && __has_feature(address_sanitizer)))
    const uintptr_t gap_lo = 0x00008fff7000ull, gap_hi = 0x02008fff6fffull;
    if (preferred_base >= gap_lo && preferred_base <= gap_hi) preferred_base = 0;
#endif
    if (preferred_base)
      p = mmap((void*)(uintptr_t)preferred_base, bytes, PROT_READ | PROT_WRITE,
               MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  }
  if (!p || p == MAP_FAILED)
    p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) return nullptr;
  if (g_extent_count < 16) {
    g_extents[g_extent_count].base = p;
    g_extents[g_extent_count].len = bytes;
    ++g_extent_count;
  }
  return p;
}
int linux_protect(void* base, size_t bytes, unsigned prot) {
  int p = 0;
  if (prot & TRT_PROT_R) p |= PROT_READ;
  if (prot & TRT_PROT_W) p |= PROT_WRITE;
  if (prot & TRT_PROT_X) p |= PROT_EXEC;
  const size_t page = (size_t)sysconf(_SC_PAGESIZE);
  const uintptr_t b = (uintptr_t)base & ~(uintptr_t)(page - 1);
  const size_t len = ((uintptr_t)base + bytes - b + page - 1) & ~(uintptr_t)(page - 1);
  return mprotect((void*)b, len, p);
}
void linux_release(void* base, size_t) {
  for (int i = 0; i < g_extent_count; ++i)
    if (g_extents[i].base == base) {
      munmap(base, g_extents[i].len);
      g_extents[i].base = nullptr;
      return;
    }
}
void linux_log(const char* line) { printf("  [trt] %s\n", line); }

u8* read_file(const char* path, size_t* size) {
  FILE* f = fopen(path, "rb");
  if (!f) return nullptr;
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  u8* buf = (u8*)malloc((size_t)n);
  const size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  *size = got;
  return buf;
}

u64 win64_call(void* fn, u64 a, u64 b, u64 c) {
  u64 args[3] = {a, b, c};
  u64 ret;
  asm volatile(
    "movq %[a0], %%rcx\n"
    "movq %[a1], %%rdx\n"
    "movq %[a2], %%r8\n"
    "subq $48, %%rsp\n"
    "call *%[fn]\n"
    "addq $48, %%rsp\n"
    : "=a"(ret)
    : [a0] "m"(args[0]), [a1] "m"(args[1]), [a2] "m"(args[2]),
      [fn] "r"(fn)
    : "rcx", "rdx", "r8", "r9", "r10", "r11",
      "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "memory", "cc");
  return ret;
}

// ---- host hook observation (what the console backend would do) ----
int g_tex_created = 0, g_buf_created = 0;
int g_cleared = 0, g_presented = 0, g_released = 0;
u32 g_rs_count = 0, g_last_rs_state = 0, g_last_rs_value = 0;
u32 g_last_fvf = 0, g_last_stage = 0, g_last_tex_h = 0;
u32 g_draw_count = 0, g_last_draw_prim = 0, g_last_draw_count = 0;
float g_vp_w = 0, g_vp_h = 0;

d3d9_handle hk_create_tex(u32 w, u32 h, u32 levels, u32 usage, u32 fmt, void* user) {
  (void)w; (void)h; (void)levels; (void)usage; (void)fmt; (void)user;
  ++g_tex_created;
  return (d3d9_handle)(0x1000 + g_tex_created);
}
d3d9_handle hk_create_buf(u32 bytes, u32 is_index, void* user) {
  (void)bytes; (void)user;
  ++g_buf_created;
  (void)is_index;
  return (d3d9_handle)(0x2000 + g_buf_created);
}
void* hk_lock(d3d9_handle h, u32 level, u32 off, u32 bytes, void* user) {
  (void)h; (void)level; (void)off; (void)bytes; (void)user;
  return nullptr;  // defer to the bridge's scratch buffer
}
void hk_unlock(d3d9_handle h, void* user) { (void)h; (void)user; }
void hk_release(d3d9_handle h, void* user) { (void)h; (void)user; ++g_released; }
void hk_clear(u32 flags, u32 color, float z, u32 stencil, void* user) {
  (void)flags; (void)color; (void)z; (void)stencil; (void)user;
  ++g_cleared;
}
void hk_present(void* user) { (void)user; ++g_presented; }
void hk_set_viewport(const d3d9_viewport* vp, void* user) {
  (void)user;
  if (vp) { g_vp_w = (float)vp->w; g_vp_h = (float)vp->h; }
}
void hk_set_texture(u32 stage, d3d9_handle h, void* user) {
  (void)user;
  g_last_stage = stage;
  g_last_tex_h = (u32)h;
}
void hk_set_transform(u32 kind, const float* m, void* user) {
  (void)kind; (void)m; (void)user;
}
void hk_set_fvf(u32 fvf, void* user) { (void)fvf; (void)user; g_last_fvf = fvf; }
void hk_set_rs(u32 state, u32 value, void* user) {
  (void)user;
  ++g_rs_count;
  g_last_rs_state = state;
  g_last_rs_value = value;
}
void hk_set_sampler(u32 stage, u32 state, u32 value, void* user) {
  (void)stage; (void)state; (void)value; (void)user;
}
void hk_draw(u32 prim, d3d9_handle vb, d3d9_handle ib, u32 base_index,
             u32 min_index, u32 num_vertices, u32 start_index, u32 prim_count,
             void* user) {
  (void)vb; (void)ib; (void)base_index; (void)min_index; (void)num_vertices;
  (void)start_index; (void)user;
  ++g_draw_count;
  g_last_draw_prim = prim;
  g_last_draw_count = prim_count;
}

// ---- Wave 2B extension hooks (shaders, decls, render targets, scissor) ----
int g_shader_created = 0, g_shader_set = 0, g_shader_const_f = 0;
int g_decl_created = 0, g_decl_set = 0;
int g_rt_set = 0, g_scissor_set = 0;
u32 g_last_rt_handle = 0;

u32 hk_create_shader(u32 is_ps, u32 maj, u32 min, const char* hlsl,
                     u32 hlsl_len, const void* parsed, void* user) {
  (void)maj; (void)min; (void)user;
  // the bridge only calls this when MojoShader parsed cleanly and produced
  // HLSL; a null/empty output here would mean a broken shader reached us
  if (!hlsl || !hlsl_len || !parsed) return 0;
  ++g_shader_created;
  return (u32)(0x3000 + g_shader_created + (is_ps ? 0x100 : 0));
}
void hk_set_shader(u32 is_ps, u32 handle, void* user) {
  (void)is_ps; (void)handle; (void)user;
  ++g_shader_set;
}
void hk_set_shader_const_f(u32 is_ps, u32 start_reg, const float* data,
                           u32 vec4_count, void* user) {
  (void)is_ps; (void)start_reg; (void)data; (void)vec4_count; (void)user;
  ++g_shader_const_f;
}
u32 hk_create_decl(const void* elems, u32 count, void* user) {
  (void)elems; (void)user;
  if (!count) return 0;
  ++g_decl_created;
  return (u32)(0x4000 + g_decl_created);
}
void hk_set_decl(u32 handle, void* user) {
  (void)handle; (void)user;
  ++g_decl_set;
}
void hk_set_render_target(u32 slot, u32 handle, void* user) {
  (void)user;
  ++g_rt_set;
  if (slot == 0) g_last_rt_handle = handle;
}
void hk_set_scissor(int enabled, int left, int top, int right, int bottom,
                    void* user) {
  (void)enabled; (void)left; (void)top; (void)right; (void)bottom; (void)user;
  ++g_scissor_set;
}

int failures = 0;
#define CHECK(cond, msg)                                             \
  do {                                                               \
    if (cond) printf("  PASS %s\n", msg);                            \
    else { printf("  FAIL %s\n", msg); ++failures; }                 \
  } while (0)

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    printf("usage: test_d3d9 <guest.exe>\n");
    return 2;
  }
  size_t size = 0;
  u8* file = read_file(argv[1], &size);
  if (!file) { printf("cannot read %s\n", argv[1]); return 2; }

  trt_backend be{&linux_alloc, &linux_protect, &linux_release, &linux_log,
                 nullptr};
  trt_bridge_boot(&be);
  trt_bridge_register_kernel32();
  trt_bridge_register_user32();
  trt_bridge_register_d3d9();

  // the host's D3D11 backend binds its hooks here (same pattern as audio)
  d3d9_hooks hk;
  memset(&hk, 0, sizeof(hk));
  hk.width = 1920;
  hk.height = 1080;
  hk.create_tex = &hk_create_tex;
  hk.create_buf = &hk_create_buf;
  hk.lock = &hk_lock;
  hk.unlock = &hk_unlock;
  hk.release = &hk_release;
  hk.clear = &hk_clear;
  hk.present = &hk_present;
  hk.set_viewport = &hk_set_viewport;
  hk.set_texture = &hk_set_texture;
  hk.set_transform = &hk_set_transform;
  hk.set_fvf = &hk_set_fvf;
  hk.set_rs = &hk_set_rs;
  hk.set_sampler = &hk_set_sampler;
  hk.draw = &hk_draw;
  hk.create_shader = &hk_create_shader;
  hk.set_shader = &hk_set_shader;
  hk.set_shader_const_f = &hk_set_shader_const_f;
  hk.create_decl = &hk_create_decl;
  hk.set_decl = &hk_set_decl;
  hk.set_render_target = &hk_set_render_target;
  hk.set_scissor = &hk_set_scissor;
  trt_d3d9_set_hooks(&hk);

  printf("tiramisu D3D9 bridge test\n");

  trt_resolver rs;
  trt_resolver_default(&rs);
  trt_image* img = nullptr;
  const int rc = trt_map_pe(&be, &rs, file, size, &img);
  CHECK(rc == TRT_OK, "guest PE maps (d3d9 bound)");
  if (rc != TRT_OK || !img) {
    printf("  map failed: %s\n", trt_errstr(rc));
    free(file);
    return 1;
  }
  CHECK(trt_seal_image(&be, img) == TRT_OK, "image sealed (W^X)");

  const u64 ret = win64_call(img->base + img->entry_rva, 0, 0, 0);
  printf("  guest returned 0x%llx (want 0x3fff)\n", (unsigned long long)ret);

  CHECK(ret & 0x001u, "Direct3DCreate9 -> device");
  CHECK(ret & 0x002u, "GetAdapterCount >= 1");
  CHECK(ret & 0x004u, "CreateDevice succeeded");
  CHECK(ret & 0x008u, "texture LockRect/UnlockRect OK");
  CHECK(ret & 0x010u, "vertex buffer Lock/Unlock OK");
  CHECK(ret & 0x020u, "render state + transform accepted");
  CHECK(ret & 0x040u, "DrawPrimitiveUP + Present OK");
  CHECK(ret & 0x080u, "BeginScene/EndScene/Clear OK");
  CHECK(ret & 0x100u, "index buffer + indexed draw OK");
  CHECK(ret & 0x200u, "GetDeviceCaps sane");
  CHECK(ret & 0x400u, "D3D9Ex: CreateDeviceEx + ResetEx + PresentEx + "
                      "CheckDeviceState + GetDisplayModeEx OK");
  CHECK(ret & 0x800u, "render target: create + set/get + backbuffer restore");
  CHECK(ret & 0x1000u, "shaders + vertex declaration (XNA SpriteBatch path)");
  CHECK(ret & 0x2000u, "sampler state + scissor rect round-trip");

  // host-side observation: the hooks actually fired
  CHECK(g_tex_created == 2, "hook: 2 textures created (tex + render target)");
  CHECK(g_buf_created == 2, "hook: 2 buffers created (vb + ib)");
  CHECK(g_cleared >= 1, "hook: Clear reached the host");
  CHECK(g_presented == 2, "hook: Present + PresentEx reached the host");
  CHECK(g_rs_count >= 3, "hook: render states marshalled");
  CHECK(g_last_fvf == 0x42, "hook: FVF (XYZ|DIFFUSE) = 0x42");
  CHECK(g_draw_count >= 2, "hook: both draws reached the host");
  CHECK(g_last_draw_prim == 4 /*TRIANGLELIST*/, "hook: prim type TRIANGLELIST");
  CHECK(g_vp_w == 1280.0f && g_vp_h == 720.0f,
        "hook: viewport = present params 1280x720");
  CHECK(g_shader_created == 2, "hook: 2 shaders created (vs + ps, parsed OK)");
  CHECK(g_shader_set == 2, "hook: both shaders bound");
  CHECK(g_shader_const_f == 2, "hook: vs + ps constants marshalled");
  CHECK(g_decl_created == 1, "hook: 1 vertex declaration created");
  CHECK(g_decl_set == 1, "hook: vertex declaration bound");
  CHECK(g_rt_set == 2, "hook: render target set + restored");
  CHECK(g_scissor_set >= 1, "hook: scissor rect reached the host");

  // Wave 2B MojoShader cbuffer reflection & packing check (vs_2_0 using c2)
  {
    static const u32 k_vs_c2[] = {
        0xFFFE0200u,                         // vs_2_0
        0x0200001Fu, 0x80000000u, 0x900F0000u, // dcl_position v0
        0x03000002u, 0xC00F0000u, 0x90E40000u, 0xA0E40002u, // add oPos, v0, c2
        0x0000FFFFu                          // end
    };
    u32 b_len = 0;
    const void* pd = trt_d3d9_mojo_parse(k_vs_c2, sizeof(k_vs_c2), &b_len);
    CHECK(pd != nullptr && b_len == sizeof(k_vs_c2),
          "Wave2B: mojo_parse parses vs_2_0 with c2 uniform");
    d3d9_mojo_layout lay{};
    u32 smask = 0;
    const u32 cb_sz = trt_d3d9_mojo_cb_info(pd, &smask);
    CHECK(trt_d3d9_mojo_extract(pd, &lay) == 1 && lay.f4_count == 1 &&
              cb_sz == 16 && lay.entry_count == 1 &&
              lay.entries[0].reg_index == 2 && lay.entries[0].cb_slot == 0,
          "Wave2B: mojo_extract maps c2 -> b0 slot 0 (16B)");
    float cf[512 * 4] = {};
    cf[2 * 4 + 0] = 11.0f;
    cf[2 * 4 + 1] = 22.0f;
    cf[2 * 4 + 2] = 33.0f;
    cf[2 * 4 + 3] = 44.0f;
    float packed[4] = {};
    CHECK(trt_d3d9_mojo_pack_cb(pd, cf, nullptr, nullptr, packed, sizeof(packed)) == 1 &&
              packed[0] == 11.0f && packed[1] == 22.0f &&
              packed[2] == 33.0f && packed[3] == 44.0f,
          "Wave2B: mojo_pack_cb packs c2 into b0 buffer cleanly");
    trt_d3d9_mojo_free(pd);
  }

  // Celeste: the guest's ps_2_0 texture shader parses cleanly (same bytes
  // the guest feeds CreatePixelShader) — extract() returns 0 on any parse
  // error, so this is the parse verdict, not just a non-null pointer.
  {
    static const u32 k_ps_tex[] = {
        0xFFFF0200u, 0x0200001Fu, 0x90000000u, 0xA00F0800u,
        0x0200001Fu, 0x80000000u, 0xB0030000u, 0x03000042u, 0x800F0000u,
        0xB0E40000u, 0xA0E40800u, 0x02000001u, 0x800F0800u, 0x80E40000u,
        0x0000FFFFu};
    u32 b_len = 0;
    const void* pd = trt_d3d9_mojo_parse(k_ps_tex, sizeof(k_ps_tex), &b_len);
    CHECK(pd != nullptr && b_len == sizeof(k_ps_tex),
          "Celeste: mojo_parse parses ps_2_0 texld shader");
    d3d9_mojo_layout lay{};
    u32 smask = 0;
    const u32 cb_sz = trt_d3d9_mojo_cb_info(pd, &smask);
    CHECK(trt_d3d9_mojo_extract(pd, &lay) == 1 && lay.f4_count == 0 &&
              cb_sz == 0 && smask == 1u,
          "Celeste: mojo_extract: ps uses sampler s0, no float constants");
    trt_d3d9_mojo_free(pd);
  }

  free(file);
  if (failures == 0) {
    printf("RESULT: ALL PASS\n");
    return 0;
  }
  printf("RESULT: %d FAILURES\n", failures);
  return 1;
}
