// tiramisu/src/trt/bridge_opengl32.h
// OpenGL 1.x-4.x & WGL -> HLSL/D3D11 translation bridge.
// Translates fixed-function GL, immediate mode, VAOs/VBOs/EBOs/UBOs/FBOs,
// GLSL 110..460 shaders (via glsl_to_hlsl), compute dispatch, and WGL context
// lifecycle onto host D3D11 hooks (src/host/gl_backend.hpp).
#ifndef TIRAMISU_BRIDGE_OPENGL32_H
#define TIRAMISU_BRIDGE_OPENGL32_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

// Primitive modes sent to host draw hook (already converted to D3D-safe modes)
enum trt_gl_prim_mode : u32 {
  TRT_GL_PRIM_POINTS = 0,
  TRT_GL_PRIM_LINES = 1,
  TRT_GL_PRIM_LINE_STRIP = 3,
  TRT_GL_PRIM_TRIANGLES = 4,
  TRT_GL_PRIM_TRIANGLE_STRIP = 5,
};

enum trt_gl_tex_fmt : u32 {
  TRT_GL_TEX_RGBA8 = 0,
  TRT_GL_TEX_BGRA8 = 1,
  TRT_GL_TEX_SRGBA8 = 2,
  TRT_GL_TEX_RGBA16F = 3,
  TRT_GL_TEX_RGBA32F = 4,
  TRT_GL_TEX_R32F = 5,
  TRT_GL_TEX_D24S8 = 6,
  TRT_GL_TEX_BC1 = 7,
  TRT_GL_TEX_BC2 = 8,
  TRT_GL_TEX_BC3 = 9,
  TRT_GL_TEX_R8 = 10,
};

enum trt_gl_tex_target : u32 {
  TRT_GL_TARGET_2D = 0,
  TRT_GL_TARGET_CUBE = 1,
  TRT_GL_TARGET_3D = 2,
  TRT_GL_TARGET_2D_ARRAY = 3,
};

enum trt_gl_buf_kind : u32 {
  TRT_GL_BUF_VERTEX = 0,
  TRT_GL_BUF_INDEX = 1,
  TRT_GL_BUF_UNIFORM = 2,
  TRT_GL_BUF_STORAGE = 3,
};

struct trt_gl_tex_desc {
  u32 target;
  u32 fmt;
  u32 w;
  u32 h;
  u32 depth;
  u32 mip_count;
  u32 min_filter;
  u32 mag_filter;
  u32 wrap_s;
  u32 wrap_t;
  u32 wrap_r;
  u32 compare_mode;
};

struct trt_gl_attrib_binding {
  u32 location;
  u32 vbo_gl;      // 0xFFFFFFFFu = use inline_vb attached to draw cmd
  u32 components;  // 1..4
  u32 gl_type;     // GL_FLOAT (0x1406), GL_UNSIGNED_BYTE (0x1401), etc.
  u8 normalized;
  u8 is_int;
  u8 divisor;
  u8 enabled;
  u32 stride;
  u32 offset;
};

struct trt_gl_render_state {
  u8 blend_en;
  u8 depth_test;
  u8 depth_write;
  u8 cull_en;
  u8 scissor_en;
  u8 stencil_en;
  u8 wireframe;
  u8 front_ccw;
  u8 color_mask;   // bit0=R, bit1=G, bit2=B, bit3=A
  u8 srgb_en;
  u8 pad[2];
  u32 blend_src_rgb;
  u32 blend_dst_rgb;
  u32 blend_src_a;
  u32 blend_dst_a;
  u32 blend_eq_rgb;
  u32 blend_eq_a;
  float blend_const[4];
  u32 depth_func;
  u32 cull_face;
  u32 stencil_func;
  int stencil_ref;
  u32 stencil_mask;
  u32 stencil_wmask;
  u32 stencil_sfail;
  u32 stencil_dpfail;
  u32 stencil_dppass;
  int vp[4];
  int sc[4];
  float depth_min;
  float depth_max;
};

struct trt_gl_sampler_slot {
  u32 gl_tex;
  u32 min_filter;
  u32 mag_filter;
  u32 wrap_s;
  u32 wrap_t;
  u32 compare_mode;
};

struct trt_gl_ubo_slot {
  u32 gl_buf;
  u32 offset;
  u32 size;
};

struct trt_gl_draw_cmd {
  u32 gl_prog;
  u32 prim_mode;   // trt_gl_prim_mode (D3D-safe)
  u8 indexed;
  u8 use_inline_idx;
  u8 pad[2];
  u32 idx_type;    // GL_UNSIGNED_SHORT (0x1403) or GL_UNSIGNED_INT (0x1405)
  u32 ibo_gl;
  u64 ibo_offset;
  const void* inline_idx;
  u32 inline_idx_bytes;
  u32 count;
  u32 first;
  u32 instance_count;
  int base_vertex;
  const void* inline_vb;
  u32 inline_vb_bytes;
  const trt_gl_attrib_binding* attribs;
  u32 attrib_count;
  u32 active_fbo;
  u32 draw_buf_count;
  u32 draw_bufs[8];
  const void* vs_cb0;
  u32 vs_cb0_bytes;
  const void* ps_cb0;
  u32 ps_cb0_bytes;
  trt_gl_sampler_slot vs_tex[16];
  u32 vs_tex_count;
  trt_gl_sampler_slot ps_tex[16];
  u32 ps_tex_count;
  trt_gl_ubo_slot ubos[14];
  u32 ubo_count;
  trt_gl_render_state rs;
};

struct trt_gl_hooks {
  void* user;
  void (*clear)(u32 mask /*bit0=color, bit1=depth, bit2=stencil*/,
                const float rgba[4], float depth, int stencil, void* user);
  void (*swap_buffers)(u64 hwnd, void* user);
  u32 (*compile_program)(u32 gl_prog, const char* vs_hlsl, u32 vs_len,
                         const char* ps_hlsl, u32 ps_len, const char* cs_hlsl,
                         u32 cs_len, void* user);
  void (*delete_program)(u32 gl_prog, void* user);
  u32 (*sync_buffer)(u32 gl_buf, u32 kind, const void* data, u32 bytes,
                     u32 version, void* user);
  void (*delete_buffer)(u32 gl_buf, void* user);
  u32 (*sync_texture)(u32 gl_tex, const trt_gl_tex_desc* desc, const void* data,
                      u32 bytes, u32 mip, u32 layer, u32 version, void* user);
  void (*gen_mipmaps)(u32 gl_tex, void* user);
  void (*delete_texture)(u32 gl_tex, void* user);
  u32 (*sync_fbo)(u32 gl_fbo, const u32* color_tex, const u32* color_mip,
                  const u32* color_layer, u32 color_count, u32 depth_tex,
                  u32 stencil_tex, void* user);
  void (*delete_fbo)(u32 gl_fbo, void* user);
  void (*read_pixels)(u32 fbo, int x, int y, int w, int h, void* out_rgba8,
                      void* user);
  void (*draw)(const trt_gl_draw_cmd* cmd, void* user);
  void (*dispatch_compute)(u32 gl_prog, u32 gx, u32 gy, u32 gz,
                           const void* cs_cb0, u32 cs_cb0_bytes,
                           const trt_gl_sampler_slot* texs, u32 tex_count,
                           const trt_gl_ubo_slot* ubos, u32 ubo_count,
                           const u32* image_uavs, u32 image_count, void* user);
};

int trt_bridge_register_opengl32(void);
void trt_gl_set_hooks(const trt_gl_hooks* hooks);
int trt_gl_render_hooks_present(void);
int trt_wgl_swap_buffers(void* hdc);

#ifdef __cplusplus
}
#endif

#endif
