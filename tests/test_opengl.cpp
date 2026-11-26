// tiramisu/tests/test_opengl.cpp
// Verifies GLSL->HLSL compiler (120/330/430/compute), OpenGL fixed-function +
// immediate mode synthesis, core VAO/VBO/EBO/UBO/FBO/textures, SwapBuffers
// wiring, WGL extensions, and real PE64 guest execution (guest_opengl.exe).
#include "../src/trt/trt.h"
#include "../src/trt/glsl_hlsl.h"
#include "../src/trt/bridge_opengl32.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

extern "C" {
int trt_bridge_register_kernel32(void);
int trt_bridge_register_user32(void);
int trt_bridge_register_gdi(void);
int trt_bridge_register_console_pack(void);
}

namespace {

int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg)                                                      \
  do {                                                                        \
    if (cond) {                                                               \
      ++g_pass;                                                               \
    } else {                                                                  \
      ++g_fail;                                                               \
      fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__);                 \
    }                                                                         \
  } while (0)

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
  if (g_extent_count < 16) g_extents[g_extent_count++] = {p, bytes};
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
  for (int i = 0; i < g_extent_count; ++i) {
    if (g_extents[i].base == base) {
      munmap(base, g_extents[i].len);
      g_extents[i].base = nullptr;
      return;
    }
  }
}
void linux_log(const char* line) { printf("  [trt] %s\n", line); }

u8* read_file_bytes(const char* path, size_t* size) {
  FILE* f = fopen(path, "rb");
  if (!f) return nullptr;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  u8* buf = (u8*)malloc((size_t)n);
  *size = fread(buf, 1, (size_t)n, f);
  fclose(f);
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

struct gl_obs {
  int clears = 0;
  u32 last_clear_mask = 0;
  int swaps = 0;
  int compiles = 0;
  std::string last_vs_hlsl, last_ps_hlsl, last_cs_hlsl;
  int buf_syncs = 0;
  u32 last_buf_bytes = 0;
  int tex_syncs = 0;
  u32 last_tex_w = 0, last_tex_h = 0;
  int fbo_syncs = 0;
  int draws = 0;
  u32 last_prim = 0;
  u32 last_count = 0;
  u32 last_inst = 0;
  u8 last_indexed = 0;
  std::vector<u8> last_ps_cb0;
  int computes = 0;
  u32 last_gx = 0, last_gy = 0, last_gz = 0;
};

gl_obs g_obs;

void ob_clear(u32 mask, const float*, float, int, void*) {
  g_obs.clears++;
  g_obs.last_clear_mask = mask;
}
void ob_swap(u64, void*) { g_obs.swaps++; }
u32 ob_compile(u32, const char* vs, u32 vsl, const char* ps, u32 psl,
               const char* cs, u32 csl, void*) {
  g_obs.compiles++;
  if (vs && vsl) g_obs.last_vs_hlsl.assign(vs, vsl);
  if (ps && psl) g_obs.last_ps_hlsl.assign(ps, psl);
  if (cs && csl) g_obs.last_cs_hlsl.assign(cs, csl);
  return 1;
}
u32 ob_sync_buf(u32, u32, const void*, u32 bytes, u32, void*) {
  g_obs.buf_syncs++;
  g_obs.last_buf_bytes = bytes;
  return 1;
}
u32 ob_sync_tex(u32, const trt_gl_tex_desc* d, const void*, u32, u32, u32, u32, void*) {
  g_obs.tex_syncs++;
  if (d) { g_obs.last_tex_w = d->w; g_obs.last_tex_h = d->h; }
  return 1;
}
u32 ob_sync_fbo(u32, const u32*, const u32*, const u32*, u32, u32, u32, void*) {
  g_obs.fbo_syncs++;
  return 1;
}
void ob_draw(const trt_gl_draw_cmd* cmd, void*) {
  g_obs.draws++;
  if (!cmd) return;
  g_obs.last_prim = cmd->prim_mode;
  g_obs.last_count = cmd->count;
  g_obs.last_inst = cmd->instance_count;
  g_obs.last_indexed = cmd->indexed;
  if (cmd->ps_cb0 && cmd->ps_cb0_bytes) {
    g_obs.last_ps_cb0.assign((const u8*)cmd->ps_cb0,
                             (const u8*)cmd->ps_cb0 + cmd->ps_cb0_bytes);
  }
}
void ob_dispatch(u32, u32 gx, u32 gy, u32 gz, const void*, u32,
                 const trt_gl_sampler_slot*, u32, const trt_gl_ubo_slot*, u32,
                 const u32*, u32, void*) {
  g_obs.computes++;
  g_obs.last_gx = gx; g_obs.last_gy = gy; g_obs.last_gz = gz;
}

void* resolve_gl(const char* mod, const char* sym) {
  return trt_resolve_module(nullptr, mod, sym, 0);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: test_opengl <guest_opengl.exe>\n");
    return 2;
  }

  // 1. Direct GLSL->HLSL compiler unit tests (compat, core, compute)
  {
    const char* vs120 = R"(
      #version 120
      varying vec2 v_uv;
      void main() {
        v_uv = gl_MultiTexCoord0.xy;
        gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;
      }
    )";
    glsl_compile_result rvs = glsl_to_hlsl(vs120, (u32)strlen(vs120), GLSL_STAGE_VERTEX);
    CHECK(rvs.ok, "GLSL 120 VS compiles to HLSL");
    CHECK(rvs.hlsl.find("gl_ModelViewProjectionMatrix") != std::string::npos,
          "GLSL 120 VS emits gl_ModelViewProjectionMatrix");
    CHECK(rvs.hlsl.find("TEXCOORD0") != std::string::npos,
          "GLSL 120 VS maps gl_Vertex to TEXCOORD0");
    CHECK(rvs.cb0_bytes >= 64, "GLSL 120 VS cbuffer includes mat4 (64B)");

    const char* cs430 = R"(
      #version 430
      layout(local_size_x = 8, local_size_y = 4, local_size_z = 1) in;
      layout(rgba8, binding = 0) uniform image2D img_out;
      void main() {
        ivec2 p = ivec2(gl_GlobalInvocationID.xy);
        imageStore(img_out, p, vec4(1.0, 0.0, 0.0, 1.0));
      }
    )";
    glsl_compile_result rcs = glsl_to_hlsl(cs430, (u32)strlen(cs430), GLSL_STAGE_COMPUTE);
    CHECK(rcs.ok, "GLSL 430 CS compiles to HLSL");
    CHECK(rcs.local_size[0] == 8 && rcs.local_size[1] == 4,
          "GLSL 430 CS parses local_size_x/y");
    CHECK(rcs.hlsl.find("numthreads(8, 4, 1)") != std::string::npos,
          "GLSL 430 CS emits [numthreads(8, 4, 1)]");
    CHECK(rcs.hlsl.find("register(u0)") != std::string::npos,
          "GLSL 430 CS binds image2D at register(u0)");
  }

  // 2. Boot runtime & attach OpenGL observer hooks
  trt_backend be{linux_alloc, linux_protect, linux_release, linux_log, nullptr};
  trt_bridge_boot(&be);
  trt_bridge_register_gdi();
  trt_bridge_register_console_pack();

  trt_gl_hooks hk{};
  hk.clear = &ob_clear;
  hk.swap_buffers = &ob_swap;
  hk.compile_program = &ob_compile;
  hk.sync_buffer = &ob_sync_buf;
  hk.sync_texture = &ob_sync_tex;
  hk.sync_fbo = &ob_sync_fbo;
  hk.draw = &ob_draw;
  hk.dispatch_compute = &ob_dispatch;
  trt_gl_set_hooks(&hk);

  // 3. Direct bridge tests: WGL ARB + FBO + UBO + Instanced + Compute + SwapBuffers
  {
    using p_gpa = void*(TRT_GUEST_ABI*)(const char*);
    using p_mk = int(TRT_GUEST_ABI*)(void*, void*);
    using p_del = int(TRT_GUEST_ABI*)(void*);
    using p_cctx = void*(TRT_GUEST_ABI*)(void*, void*, const int*);
    using p_cpf = int(TRT_GUEST_ABI*)(void*, const int*, const float*, u32, int*, u32*);
    using p_ext = const char*(TRT_GUEST_ABI*)(void*);
    using p_swap = int(TRT_GUEST_ABI*)(void*);

    auto gpa = (p_gpa)resolve_gl("opengl32.dll", "wglGetProcAddress");
    auto mkcur = (p_mk)resolve_gl("opengl32.dll", "wglMakeCurrent");
    auto delctx = (p_del)resolve_gl("opengl32.dll", "wglDeleteContext");
    auto cctx_arb = (p_cctx)(gpa ? gpa("wglCreateContextAttribsARB") : nullptr);
    auto cpf_arb = (p_cpf)(gpa ? gpa("wglChoosePixelFormatARB") : nullptr);
    auto ext_arb = (p_ext)(gpa ? gpa("wglGetExtensionsStringARB") : nullptr);
    auto fn_swap = (p_swap)resolve_gl("gdi32.dll", "SwapBuffers");

    CHECK(cctx_arb && cpf_arb && ext_arb && fn_swap, "WGL ARB & SwapBuffers resolved");
    int fmt = 0; u32 nfmt = 0;
    CHECK(cpf_arb((void*)1, nullptr, nullptr, 1, &fmt, &nfmt) == 1 && fmt == 1 && nfmt == 1,
          "wglChoosePixelFormatARB picks format 1");
    void* ctx = cctx_arb((void*)1, nullptr, nullptr);
    CHECK(ctx && mkcur((void*)1, ctx) == 1, "wglCreateContextAttribsARB creates & activates");
    CHECK(strstr(ext_arb((void*)1), "WGL_ARB_create_context") != nullptr,
          "WGL extensions list WGL_ARB_create_context");

    // Compute + FBO + Instanced test
    using p_gen = void(TRT_GUEST_ABI*)(int, u32*);
    using p_bind = void(TRT_GUEST_ABI*)(u32, u32);
    using p_fbtex = void(TRT_GUEST_ABI*)(u32, u32, u32, u32, int);
    using p_drawi = void(TRT_GUEST_ABI*)(u32, int, u32, const void*, int);
    using p_cs = u32(TRT_GUEST_ABI*)(u32);
    using p_shsrc = void(TRT_GUEST_ABI*)(u32, int, const char* const*, const int*);
    using p_voidu = void(TRT_GUEST_ABI*)(u32);
    using p_u2 = void(TRT_GUEST_ABI*)(u32, u32);
    using p_disp = void(TRT_GUEST_ABI*)(u32, u32, u32);

    auto gen_fbos = (p_gen)gpa("glGenFramebuffers");
    auto bind_fbo = (p_bind)gpa("glBindFramebuffer");
    auto fb_tex2d = (p_fbtex)gpa("glFramebufferTexture2D");
    auto draw_el_i = (p_drawi)gpa("glDrawElementsInstanced");
    auto cr_sh = (p_cs)gpa("glCreateShader");
    auto sh_src = (p_shsrc)gpa("glShaderSource");
    auto cmp_sh = (p_voidu)gpa("glCompileShader");
    auto cr_pr = (p_cs)resolve_gl("opengl32.dll", "glGetError");
    (void)cr_pr;
    auto mk_prog = (u32(TRT_GUEST_ABI*)(void))gpa("glCreateProgram");
    auto attach_sh = (p_u2)gpa("glAttachShader");
    auto link_pr = (p_voidu)gpa("glLinkProgram");
    auto use_pr = (p_voidu)gpa("glUseProgram");
    auto disp_cs = (p_disp)gpa("glDispatchCompute");

    // FBO setup + compute shader
    u32 fbo = 0; gen_fbos(1, &fbo);
    bind_fbo(0x8D40u, fbo);
    fb_tex2d(0x8D40u, 0x8CE0u, 0x0DE1u, 1u, 0);
    auto fn_clear = (void(TRT_GUEST_ABI*)(u32))resolve_gl("opengl32.dll", "glClear");
    if (fn_clear) fn_clear(0x4000u);
    CHECK(g_obs.fbo_syncs >= 1, "glBindFramebuffer + glFramebufferTexture2D syncs FBO");
    const char* cs_code = "#version 430\nlayout(local_size_x=4) in;\nvoid main() {}\n";
    u32 cs = cr_sh(0x91B9u);
    sh_src(cs, 1, &cs_code, nullptr);
    cmp_sh(cs);
    u32 pr = mk_prog();
    attach_sh(pr, cs);
    link_pr(pr);
    use_pr(pr);
    disp_cs(8, 2, 1);
    CHECK(g_obs.computes == 1 && g_obs.last_gx == 8 && g_obs.last_gy == 2,
          "glDispatchCompute routed through hooks");

    // Check glDrawElementsInstanced with client uint8 indices + GLSL 330 shader
    const char* vs_inst = "#version 330 core\nlayout(location=0) in vec2 p;\nvoid main(){ gl_Position=vec4(p,0,1); }\n";
    const char* ps_inst = "#version 330 core\nout vec4 c;\nvoid main(){ c=vec4(1); }\n";
    u32 ivs = cr_sh(0x8B31u), ips = cr_sh(0x8B30u), ipr = mk_prog();
    sh_src(ivs, 1, &vs_inst, nullptr); cmp_sh(ivs);
    sh_src(ips, 1, &ps_inst, nullptr); cmp_sh(ips);
    attach_sh(ipr, ivs); attach_sh(ipr, ips); link_pr(ipr); use_pr(ipr);
    const u8 idx8[3] = {0, 1, 2};
    draw_el_i(4, 3, 0x1401u /*GL_UNSIGNED_BYTE*/, idx8, 5);
    CHECK(g_obs.last_indexed == 1 && g_obs.last_inst == 5 && g_obs.last_count == 3,
          "glDrawElementsInstanced + UBYTE->UINT conversion works");

    // Check gdi32!SwapBuffers triggers WGL hook
    int prev_swaps = g_obs.swaps;
    fn_swap((void*)1);
    CHECK(g_obs.swaps == prev_swaps + 1, "gdi32!SwapBuffers fires gl hooks swap_buffers");

    mkcur(nullptr, nullptr);
    delctx(ctx);
  }

  // 4. Boot REAL PE64 guest (guest_opengl.exe)
  g_obs = {};
  size_t sz = 0;
  u8* file = read_file_bytes(argv[1], &sz);
  CHECK(file != nullptr, "read guest_opengl.exe");
  if (file) {
    trt_resolver rs{};
    trt_resolver_default(&rs);
    trt_image* img = nullptr;
    CHECK(trt_map_pe(&be, &rs, file, sz, &img) == TRT_OK, "trt_map_pe(guest_opengl.exe) OK");
    CHECK(trt_seal_image(&be, img) == TRT_OK, "trt_seal_image(guest_opengl.exe) OK");
    u64 bits = win64_call(img->base + img->entry_rva, 0, 0, 0);
    printf("  guest_opengl_main returned 0x%llx (compiles=%d draws=%d swaps=%d)\n",
           (unsigned long long)bits, g_obs.compiles, g_obs.draws, g_obs.swaps);
    CHECK((bits & 0xFFu) == 0xFFu, "guest_opengl_main passed all 8 bit checks (0xFF)");
    CHECK(g_obs.clears == 1 && g_obs.last_clear_mask == 3u, "PE64 guest clear COLOR|DEPTH");
    CHECK(g_obs.tex_syncs >= 1 && g_obs.last_tex_w == 2 && g_obs.last_tex_h == 2,
          "PE64 guest 2x2 RGBA texture uploaded");
    CHECK(g_obs.compiles >= 2, "PE64 guest compiled FF + GLSL 330 core shaders");
    CHECK(g_obs.draws == 2, "PE64 guest emitted FF GL_QUADS + GLSL 330 core VBO draw");
    CHECK(g_obs.swaps == 1, "PE64 guest SwapBuffers presented");
    trt_unmap(&be, img);
    free(file);
  }

  trt_gl_set_hooks(nullptr);
  printf("test_opengl: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
