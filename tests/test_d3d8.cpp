// tiramisu/tests/test_d3d8.cpp
// The D3D8 bridge: a guest goes through the D3D8 lifecycle (device, texture,
// vertex/index buffers, locks, state, transform, draw, present, DWORD-handle
// shaders, SetIndices base vertex, CopyRects) and the host hooks observe
// every GPU action.
#include "../src/trt/trt.h"
#include "../src/trt/bridge_d3d9.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

extern "C" {
int trt_bridge_register_kernel32(void);
int trt_bridge_register_user32(void);
int trt_bridge_register_d3d8(void);
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
int g_cleared = 0, g_presented = 0;
u32 g_rs_count = 0;
u32 g_last_fvf = 0;
u32 g_draw_count = 0, g_last_draw_prim = 0, g_last_draw_count = 0;
u32 g_last_base_index = 0xFFFFFFFF;
float g_vp_w = 0, g_vp_h = 0;

d3d9_handle hk_create_tex(u32 w, u32 h, u32 levels, u32 usage, u32 fmt, void* user) {
  (void)w; (void)h; (void)levels; (void)usage; (void)fmt; (void)user;
  ++g_tex_created;
  return (d3d9_handle)(0x1000 + g_tex_created);
}
d3d9_handle hk_create_buf(u32 bytes, u32 is_index, void* user) {
  (void)bytes; (void)is_index; (void)user;
  ++g_buf_created;
  return (d3d9_handle)(0x2000 + g_buf_created);
}
void* hk_lock(d3d9_handle h, u32 level, u32 off, u32 bytes, void* user) {
  (void)h; (void)level; (void)off; (void)bytes; (void)user;
  return nullptr;  // defer to the bridge's scratch buffer
}
void hk_unlock(d3d9_handle h, void* user) { (void)h; (void)user; }
void hk_release(d3d9_handle h, void* user) { (void)h; (void)user; }
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
  (void)stage; (void)h; (void)user;
}
void hk_set_transform(u32 kind, const float* m, void* user) {
  (void)kind; (void)m; (void)user;
}
void hk_set_fvf(u32 fvf, void* user) { (void)fvf; (void)user; g_last_fvf = fvf; }
void hk_set_rs(u32 state, u32 value, void* user) {
  (void)user;
  ++g_rs_count;
}
void hk_set_sampler(u32 stage, u32 state, u32 value, void* user) {
  (void)stage; (void)state; (void)value; (void)user;
}
void hk_draw(u32 prim, d3d9_handle vb, d3d9_handle ib, u32 base_index,
             u32 min_index, u32 num_vertices, u32 start_index, u32 prim_count,
             void* user) {
  (void)vb; (void)min_index; (void)num_vertices; (void)start_index;
  (void)user;
  ++g_draw_count;
  g_last_draw_prim = prim;
  g_last_draw_count = prim_count;
  if (ib) g_last_base_index = base_index;
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
    printf("usage: test_d3d8 <guest.exe>\n");
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
  trt_bridge_register_d3d8();

  // the host's D3D11 backend binds its hooks here (same pattern as d3d9)
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
  trt_d3d9_set_hooks(&hk);

  printf("tiramisu D3D8 bridge test\n");

  trt_resolver rs;
  trt_resolver_default(&rs);
  trt_image* img = nullptr;
  const int rc = trt_map_pe(&be, &rs, file, size, &img);
  CHECK(rc == TRT_OK, "guest PE maps (d3d8 bound)");
  if (rc != TRT_OK || !img) {
    printf("  map failed: %s\n", trt_errstr(rc));
    free(file);
    return 1;
  }
  CHECK(trt_seal_image(&be, img) == TRT_OK, "image sealed (W^X)");

  const u64 ret = win64_call(img->base + img->entry_rva, 0, 0, 0);
  printf("  guest returned 0x%llx (want 0xfff)\n", (unsigned long long)ret);

  CHECK(ret & 0x001u, "Direct3DCreate8 -> object");
  CHECK(ret & 0x002u, "GetAdapterCount >= 1");
  CHECK(ret & 0x004u, "CreateDevice succeeded (slot 15)");
  CHECK(ret & 0x008u, "texture LockRect/UnlockRect OK");
  CHECK(ret & 0x010u, "vertex buffer Lock/Unlock OK");
  CHECK(ret & 0x020u, "render state + transform + FVF accepted");
  CHECK(ret & 0x040u, "DrawPrimitiveUP + Present OK");
  CHECK(ret & 0x080u, "BeginScene/EndScene/Clear OK");
  CHECK(ret & 0x100u, "index buffer + SetIndices(base) + indexed draw OK");
  CHECK(ret & 0x200u, "GetDeviceCaps sane (D3DCAPS8 layout)");
  CHECK(ret & 0x400u, "CreateVertexShader -> DWORD handle");
  CHECK(ret & 0x800u, "CopyRects (D3D8-only) OK");

  // host-side observation: the hooks actually fired
  CHECK(g_tex_created == 1, "hook: 1 texture created");
  CHECK(g_buf_created == 2, "hook: 2 buffers created (vb + ib)");
  CHECK(g_cleared >= 1, "hook: Clear reached the host");
  CHECK(g_presented == 1, "hook: Present reached the host");
  CHECK(g_rs_count >= 2, "hook: render states marshalled");
  CHECK(g_last_fvf == 0x42, "hook: FVF via shader handle = 0x42");
  CHECK(g_draw_count >= 2, "hook: both draws reached the host");
  CHECK(g_last_draw_prim == 4 /*TRIANGLELIST*/, "hook: prim type TRIANGLELIST");
  CHECK(g_last_base_index == 16, "hook: D3D8 SetIndices base vertex = 16");
  CHECK(g_vp_w == 1280.0f && g_vp_h == 720.0f,
        "hook: viewport = present params 1280x720");

  free(file);
  if (failures) {
    printf("RESULT: %d FAIL\n", failures);
    return 1;
  }
  printf("RESULT: ALL PASS\n");
  return 0;
}
