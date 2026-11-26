// tiramisu/src/trt/bridge_opengl32.cpp
// Real OpenGL 1.x-4.x & WGL -> HLSL/D3D11 translation bridge.
// Implements WGL context/extension lifecycle, OpenGL matrix stacks, fixed-function
// vertex/fragment synthesis, immediate mode & display lists, VAOs/VBOs/EBOs/UBOs,
// FBOs/RBOs/textures/samplers, GLSL->HLSL compilation (via glsl_to_hlsl),
// uniform/attrib/UBO/sampler reflection, compute dispatch, and 180+ GL/WGL procs.
#include "bridge_opengl32.h"
#include "glsl_hlsl.h"
#include "thunk.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Note: kernel32 handles handle lookup if needed; bridge_opengl32 tracks
// GL objects + WGL contexts directly in-process.
namespace {

static std::mutex g_gl_mu;
static trt_gl_hooks g_gl_hooks = {};
static bool g_gl_hooks_set = false;

static void opengl_log(const char* msg) {
  trt_core_log(msg);
}

// ---- GL / WGL constants (subset matching OpenGL 1.1..4.6 headers) ---------
static constexpr u32 GL_NO_ERROR_V = 0;
static constexpr u32 GL_INVALID_ENUM_V = 0x0500;
static constexpr u32 GL_INVALID_VALUE_V = 0x0501;
static constexpr u32 GL_INVALID_OPERATION_V = 0x0502;
static constexpr u32 GL_STACK_OVERFLOW_V = 0x0503;
static constexpr u32 GL_STACK_UNDERFLOW_V = 0x0504;
static constexpr u32 GL_OUT_OF_MEMORY_V = 0x0505;
static constexpr u32 GL_INVALID_FRAMEBUFFER_OPERATION_V = 0x0506;

static constexpr u32 GL_FALSE_V = 0;
static constexpr u32 GL_TRUE_V = 1;

static constexpr u32 GL_DEPTH_BUFFER_BIT_V = 0x00000100;
static constexpr u32 GL_STENCIL_BUFFER_BIT_V = 0x00000400;
static constexpr u32 GL_COLOR_BUFFER_BIT_V = 0x00004000;

static constexpr u32 GL_POINTS_V = 0x0000;
static constexpr u32 GL_LINES_V = 0x0001;
static constexpr u32 GL_LINE_LOOP_V = 0x0002;
static constexpr u32 GL_LINE_STRIP_V = 0x0003;
static constexpr u32 GL_TRIANGLES_V = 0x0004;
static constexpr u32 GL_TRIANGLE_STRIP_V = 0x0005;
static constexpr u32 GL_TRIANGLE_FAN_V = 0x0006;
static constexpr u32 GL_QUADS_V = 0x0007;
static constexpr u32 GL_QUAD_STRIP_V = 0x0008;
static constexpr u32 GL_POLYGON_V = 0x0009;

static constexpr u32 GL_NEVER_V = 0x0200;
static constexpr u32 GL_LESS_V = 0x0201;
static constexpr u32 GL_EQUAL_V = 0x0202;
static constexpr u32 GL_LEQUAL_V = 0x0203;
static constexpr u32 GL_GREATER_V = 0x0204;
static constexpr u32 GL_NOTEQUAL_V = 0x0205;
static constexpr u32 GL_GEQUAL_V = 0x0206;
static constexpr u32 GL_ALWAYS_V = 0x0207;

static constexpr u32 GL_ZERO_V = 0;
static constexpr u32 GL_ONE_V = 1;
static constexpr u32 GL_SRC_COLOR_V = 0x0300;
static constexpr u32 GL_ONE_MINUS_SRC_COLOR_V = 0x0301;
static constexpr u32 GL_SRC_ALPHA_V = 0x0302;
static constexpr u32 GL_ONE_MINUS_SRC_ALPHA_V = 0x0303;
static constexpr u32 GL_DST_ALPHA_V = 0x0304;
static constexpr u32 GL_ONE_MINUS_DST_ALPHA_V = 0x0305;
static constexpr u32 GL_DST_COLOR_V = 0x0306;
static constexpr u32 GL_ONE_MINUS_DST_COLOR_V = 0x0307;

static constexpr u32 GL_FRONT_V = 0x0404;
static constexpr u32 GL_BACK_V = 0x0405;
static constexpr u32 GL_FRONT_AND_BACK_V = 0x0408;

static constexpr u32 GL_CW_V = 0x0900;
static constexpr u32 GL_CCW_V = 0x0901;

static constexpr u32 GL_FOG_V = 0x0B60;
static constexpr u32 GL_LIGHTING_V = 0x0B50;
static constexpr u32 GL_DEPTH_TEST_V = 0x0B71;
static constexpr u32 GL_STENCIL_TEST_V = 0x0B90;
static constexpr u32 GL_ALPHA_TEST_V = 0x0BC0;
static constexpr u32 GL_BLEND_V = 0x0BE2;
static constexpr u32 GL_CULL_FACE_V = 0x0B44;
static constexpr u32 GL_SCISSOR_TEST_V = 0x0C11;
static constexpr u32 GL_TEXTURE_2D_V = 0x0DE1;
static constexpr u32 GL_TEXTURE_3D_V = 0x806F;
static constexpr u32 GL_TEXTURE_CUBE_MAP_V = 0x8513;
static constexpr u32 GL_TEXTURE_CUBE_MAP_POS_X = 0x8515;
static constexpr u32 GL_TEXTURE_CUBE_MAP_NEG_Z = 0x851A;
static constexpr u32 GL_TEXTURE_2D_ARRAY_V = 0x8C1A;
static constexpr u32 GL_FRAMEBUFFER_SRGB_V = 0x8DB9;

static constexpr u32 GL_BYTE_V = 0x1400;
static constexpr u32 GL_UNSIGNED_BYTE_V = 0x1401;
static constexpr u32 GL_SHORT_V = 0x1402;
static constexpr u32 GL_UNSIGNED_SHORT_V = 0x1403;
static constexpr u32 GL_INT_V = 0x1404;
static constexpr u32 GL_UNSIGNED_INT_V = 0x1405;
static constexpr u32 GL_FLOAT_V = 0x1406;
static constexpr u32 GL_UNSIGNED_SHORT_5_6_5_V = 0x8363;
static constexpr u32 GL_UNSIGNED_SHORT_4_4_4_4_V = 0x8033;
static constexpr u32 GL_UNSIGNED_SHORT_5_5_5_1_V = 0x8034;
static constexpr u32 GL_UNSIGNED_INT_24_8_V = 0x84FA;

static constexpr u32 GL_DEPTH_COMPONENT_V = 0x1902;
static constexpr u32 GL_RED_V = 0x1903;
static constexpr u32 GL_RGB_V = 0x1907;
static constexpr u32 GL_RGBA_V = 0x1908;
static constexpr u32 GL_LUMINANCE_V = 0x1909;
static constexpr u32 GL_LUMINANCE_ALPHA_V = 0x190A;
static constexpr u32 GL_BGR_V = 0x80E0;
static constexpr u32 GL_BGRA_V = 0x80E1;
static constexpr u32 GL_RG_V = 0x8227;
static constexpr u32 GL_DEPTH_STENCIL_V = 0x84F9;

static constexpr u32 GL_RGBA8_V = 0x8058;
static constexpr u32 GL_RGB8_V = 0x8051;
static constexpr u32 GL_R8_V = 0x8229;
static constexpr u32 GL_RG8_V = 0x822B;
static constexpr u32 GL_RGBA16F_V = 0x881A;
static constexpr u32 GL_RGBA32F_V = 0x8814;
static constexpr u32 GL_R32F_V = 0x822E;
static constexpr u32 GL_SRGB8_V = 0x8C41;
static constexpr u32 GL_SRGB8_ALPHA8_V = 0x8C43;
static constexpr u32 GL_DEPTH24_STENCIL8_V = 0x88F0;
static constexpr u32 GL_DEPTH_COMPONENT24_V = 0x81A6;
static constexpr u32 GL_DEPTH_COMPONENT32F_V = 0x8CAC;
static constexpr u32 GL_COMPRESSED_RGB_S3TC_DXT1_V = 0x83F0;
static constexpr u32 GL_COMPRESSED_RGBA_S3TC_DXT1_V = 0x83F1;
static constexpr u32 GL_COMPRESSED_RGBA_S3TC_DXT3_V = 0x83F2;
static constexpr u32 GL_COMPRESSED_RGBA_S3TC_DXT5_V = 0x83F3;

static constexpr u32 GL_TEXTURE_MAG_FILTER_V = 0x2800;
static constexpr u32 GL_TEXTURE_MIN_FILTER_V = 0x2801;
static constexpr u32 GL_TEXTURE_WRAP_S_V = 0x2802;
static constexpr u32 GL_TEXTURE_WRAP_T_V = 0x2803;
static constexpr u32 GL_TEXTURE_WRAP_R_V = 0x8072;
static constexpr u32 GL_TEXTURE_COMPARE_MODE_V = 0x884C;
static constexpr u32 GL_NEAREST_V = 0x2600;
static constexpr u32 GL_LINEAR_V = 0x2601;
static constexpr u32 GL_NEAREST_MIPMAP_NEAREST_V = 0x2700;
static constexpr u32 GL_LINEAR_MIPMAP_NEAREST_V = 0x2701;
static constexpr u32 GL_NEAREST_MIPMAP_LINEAR_V = 0x2702;
static constexpr u32 GL_LINEAR_MIPMAP_LINEAR_V = 0x2703;
static constexpr u32 GL_REPEAT_V = 0x2901;
static constexpr u32 GL_CLAMP_TO_EDGE_V = 0x812F;

static constexpr u32 GL_MODELVIEW_V = 0x1700;
static constexpr u32 GL_PROJECTION_V = 0x1701;
static constexpr u32 GL_TEXTURE_V = 0x1702;

static constexpr u32 GL_VERTEX_ARRAY_V = 0x8074;
static constexpr u32 GL_NORMAL_ARRAY_V = 0x8075;
static constexpr u32 GL_COLOR_ARRAY_V = 0x8076;
static constexpr u32 GL_TEXTURE_COORD_ARRAY_V = 0x8078;

static constexpr u32 GL_LINE_V = 0x1B01;
static constexpr u32 GL_FILL_V = 0x1B02;
static constexpr u32 GL_KEEP_V = 0x1E00;
static constexpr u32 GL_FUNC_ADD_V = 0x8006;

static constexpr u32 GL_VENDOR_V = 0x1F00;
static constexpr u32 GL_RENDERER_V = 0x1F01;
static constexpr u32 GL_VERSION_V = 0x1F02;
static constexpr u32 GL_EXTENSIONS_V = 0x1F03;
static constexpr u32 GL_SHADING_LANGUAGE_VERSION_V = 0x8B8C;

static constexpr u32 GL_ARRAY_BUFFER_V = 0x8892;
static constexpr u32 GL_ELEMENT_ARRAY_BUFFER_V = 0x8893;
static constexpr u32 GL_PIXEL_UNPACK_BUFFER_V = 0x88EC;
static constexpr u32 GL_UNIFORM_BUFFER_V = 0x8A11;
static constexpr u32 GL_SHADER_STORAGE_BUFFER_V = 0x90D2;
static constexpr u32 GL_COPY_READ_BUFFER_V = 0x8F36;
static constexpr u32 GL_COPY_WRITE_BUFFER_V = 0x8F37;

static constexpr u32 GL_FRAGMENT_SHADER_V = 0x8B30;
static constexpr u32 GL_VERTEX_SHADER_V = 0x8B31;
static constexpr u32 GL_COMPUTE_SHADER_V = 0x91B9;
static constexpr u32 GL_SHADER_TYPE_V = 0x8B4F;
static constexpr u32 GL_DELETE_STATUS_V = 0x8B80;
static constexpr u32 GL_COMPILE_STATUS_V = 0x8B81;
static constexpr u32 GL_LINK_STATUS_V = 0x8B82;
static constexpr u32 GL_VALIDATE_STATUS_V = 0x8B83;
static constexpr u32 GL_INFO_LOG_LENGTH_V = 0x8B84;
static constexpr u32 GL_ATTACHED_SHADERS_V = 0x8B85;
static constexpr u32 GL_ACTIVE_UNIFORMS_V = 0x8B86;
static constexpr u32 GL_ACTIVE_UNIFORM_MAX_LENGTH_V = 0x8B87;
static constexpr u32 GL_SHADER_SOURCE_LENGTH_V = 0x8B88;
static constexpr u32 GL_ACTIVE_ATTRIBUTES_V = 0x8B89;
static constexpr u32 GL_ACTIVE_ATTRIBUTE_MAX_LENGTH_V = 0x8B8A;
static constexpr u32 GL_ACTIVE_UNIFORM_BLOCKS_V = 0x8A36;
static constexpr u32 GL_UNIFORM_BLOCK_DATA_SIZE_V = 0x8A40;
static constexpr u32 GL_UNIFORM_BLOCK_NAME_LENGTH_V = 0x8A41;

static constexpr u32 GL_FRAMEBUFFER_V = 0x8D40;
static constexpr u32 GL_READ_FRAMEBUFFER_V = 0x8CA8;
static constexpr u32 GL_DRAW_FRAMEBUFFER_V = 0x8CA9;
static constexpr u32 GL_RENDERBUFFER_V = 0x8D41;
static constexpr u32 GL_COLOR_ATTACHMENT0_V = 0x8CE0;
static constexpr u32 GL_DEPTH_ATTACHMENT_V = 0x8D00;
static constexpr u32 GL_STENCIL_ATTACHMENT_V = 0x8D20;
static constexpr u32 GL_DEPTH_STENCIL_ATTACHMENT_V = 0x821A;
static constexpr u32 GL_FRAMEBUFFER_COMPLETE_V = 0x8CD5;

static constexpr u32 GL_ACTIVE_TEXTURE_V = 0x84E0;
static constexpr u32 GL_MAX_TEXTURE_SIZE_V = 0x0D33;
static constexpr u32 GL_MAX_TEXTURE_IMAGE_UNITS_V = 0x8872;
static constexpr u32 GL_MAX_VERTEX_ATTRIBS_V = 0x8869;
static constexpr u32 GL_MAX_COLOR_ATTACHMENTS_V = 0x8CDF;
static constexpr u32 GL_MAX_DRAW_BUFFERS_V = 0x8824;
static constexpr u32 GL_NUM_EXTENSIONS_V = 0x821D;
static constexpr u32 GL_VIEWPORT_V = 0x0BA2;
static constexpr u32 GL_SCISSOR_BOX_V = 0x0C10;
static constexpr u32 GL_CURRENT_PROGRAM_V = 0x8B8D;
static constexpr u32 GL_VERTEX_ARRAY_BINDING_V = 0x85B5;
static constexpr u32 GL_ARRAY_BUFFER_BINDING_V = 0x8894;
static constexpr u32 GL_ELEMENT_ARRAY_BUFFER_BINDING_V = 0x8895;
static constexpr u32 GL_FRAMEBUFFER_BINDING_V = 0x8CA6;
static constexpr u32 GL_TEXTURE_BINDING_2D_V = 0x8069;
static constexpr u32 GL_MAJOR_VERSION_V = 0x821B;
static constexpr u32 GL_MINOR_VERSION_V = 0x821C;

static constexpr u32 GL_UNPACK_ROW_LENGTH_V = 0x0CF2;
static constexpr u32 GL_UNPACK_SKIP_ROWS_V = 0x0CF3;
static constexpr u32 GL_UNPACK_SKIP_PIXELS_V = 0x0CF4;
static constexpr u32 GL_UNPACK_ALIGNMENT_V = 0x0CF5;
static constexpr u32 GL_PACK_ALIGNMENT_V = 0x0D05;

static constexpr u32 GL_FOG_MODE_V = 0x0B65;
static constexpr u32 GL_FOG_DENSITY_V = 0x0B62;
static constexpr u32 GL_FOG_START_V = 0x0B63;
static constexpr u32 GL_FOG_END_V = 0x0B64;
static constexpr u32 GL_FOG_COLOR_V = 0x0B66;
static constexpr u32 GL_EXP_V = 0x0800;
static constexpr u32 GL_EXP2_V = 0x0801;
static constexpr u32 GL_LINEAR_FOG_V = 0x2601;

static constexpr u32 GL_AMBIENT_V = 0x1200;
static constexpr u32 GL_DIFFUSE_V = 0x1201;
static constexpr u32 GL_SPECULAR_V = 0x1202;
static constexpr u32 GL_POSITION_V = 0x1203;
static constexpr u32 GL_LIGHT0_V = 0x4000;
static constexpr u32 GL_LIGHT_MODEL_AMBIENT_V = 0x0B53;

static constexpr u32 GL_SYNC_GPU_COMMANDS_COMPLETE_V = 0x9117;
static constexpr u32 GL_ALREADY_SIGNALED_V = 0x911A;
static constexpr u32 GL_CONDITION_SATISFIED_V = 0x911C;
static constexpr u32 GL_SYNC_STATUS_V = 0x9114;
static constexpr u32 GL_SIGNALED_V = 0x9119;
static constexpr u32 GL_QUERY_RESULT_V = 0x8866;
static constexpr u32 GL_QUERY_RESULT_AVAILABLE_V = 0x8867;

static constexpr u32 GL_FF_PROG_BASE = 0x80000000u;

static const char* k_gl_exts[] = {
    "GL_ARB_vertex_buffer_object",
    "GL_ARB_vertex_array_object",
    "GL_ARB_framebuffer_object",
    "GL_EXT_framebuffer_object",
    "GL_EXT_framebuffer_blit",
    "GL_ARB_shader_objects",
    "GL_ARB_vertex_shader",
    "GL_ARB_fragment_shader",
    "GL_ARB_shading_language_100",
    "GL_ARB_uniform_buffer_object",
    "GL_ARB_compute_shader",
    "GL_ARB_shader_image_load_store",
    "GL_ARB_instanced_arrays",
    "GL_ARB_draw_instanced",
    "GL_ARB_draw_elements_base_vertex",
    "GL_ARB_base_instance",
    "GL_ARB_sampler_objects",
    "GL_ARB_texture_storage",
    "GL_ARB_texture_non_power_of_two",
    "GL_ARB_multitexture",
    "GL_ARB_pixel_buffer_object",
    "GL_ARB_map_buffer_range",
    "GL_ARB_sync",
    "GL_ARB_framebuffer_sRGB",
    "GL_EXT_texture_compression_s3tc",
    "GL_EXT_texture_filter_anisotropic",
    "GL_KHR_debug",
};

static std::string build_ext_string() {
  std::string s;
  for (size_t i = 0; i < sizeof(k_gl_exts) / sizeof(k_gl_exts[0]); ++i) {
    if (i) s.push_back(' ');
    s += k_gl_exts[i];
  }
  return s;
}

// ---- Math helpers (column-major 4x4) --------------------------------------
struct gl_mat4 {
  float m[16];
};

static gl_mat4 mat4_identity() {
  gl_mat4 r{};
  r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
  return r;
}

static gl_mat4 mat4_mul(const gl_mat4& a, const gl_mat4& b) {
  gl_mat4 r{};
  for (int col = 0; col < 4; ++col) {
    for (int row = 0; row < 4; ++row) {
      float s = 0.0f;
      for (int k = 0; k < 4; ++k) s += a.m[k * 4 + row] * b.m[col * 4 + k];
      r.m[col * 4 + row] = s;
    }
  }
  return r;
}

static void mat4_normal3x3(const gl_mat4& mv, float out3x4[12]) {
  // Inverse transpose of upper-left 3x3 of mv, stored as 3 float4 columns
  const float a00 = mv.m[0], a01 = mv.m[4], a02 = mv.m[8];
  const float a10 = mv.m[1], a11 = mv.m[5], a12 = mv.m[9];
  const float a20 = mv.m[2], a21 = mv.m[6], a22 = mv.m[10];
  const float c00 = a11 * a22 - a12 * a21;
  const float c01 = a12 * a20 - a10 * a22;
  const float c02 = a10 * a21 - a11 * a20;
  const float det = a00 * c00 + a01 * c01 + a02 * c02;
  if (fabsf(det) < 1e-12f) {
    memset(out3x4, 0, sizeof(float) * 12);
    out3x4[0] = out3x4[5] = out3x4[10] = 1.0f;
    return;
  }
  const float inv = 1.0f / det;
  // Row/column of inverse-transpose:
  out3x4[0] = c00 * inv;
  out3x4[1] = c01 * inv;
  out3x4[2] = c02 * inv;
  out3x4[3] = 0.0f;
  out3x4[4] = (a02 * a21 - a01 * a22) * inv;
  out3x4[5] = (a00 * a22 - a02 * a20) * inv;
  out3x4[6] = (a01 * a20 - a00 * a21) * inv;
  out3x4[7] = 0.0f;
  out3x4[8] = (a01 * a12 - a02 * a11) * inv;
  out3x4[9] = (a02 * a10 - a00 * a12) * inv;
  out3x4[10] = (a00 * a11 - a01 * a10) * inv;
  out3x4[11] = 0.0f;
}

static u32 gl_type_bytes(u32 t) {
  switch (t) {
    case GL_BYTE_V:
    case GL_UNSIGNED_BYTE_V: return 1;
    case GL_SHORT_V:
    case GL_UNSIGNED_SHORT_V: return 2;
    case GL_INT_V:
    case GL_UNSIGNED_INT_V:
    case GL_FLOAT_V: return 4;
    default: return 4;
  }
}

static u32 hlsl_val_kind_comp_bytes(glsl_val_kind k) {
  switch (k) {
    case GLSL_T_FLOAT: case GLSL_T_INT: case GLSL_T_UINT: case GLSL_T_BOOL: return 4;
    case GLSL_T_VEC2: case GLSL_T_IVEC2: case GLSL_T_UVEC2: case GLSL_T_BVEC2: return 8;
    case GLSL_T_VEC3: case GLSL_T_IVEC3: case GLSL_T_UVEC3: case GLSL_T_BVEC3: return 12;
    case GLSL_T_VEC4: case GLSL_T_IVEC4: case GLSL_T_UVEC4: case GLSL_T_BVEC4: return 16;
    case GLSL_T_MAT2: return 32;
    case GLSL_T_MAT3: return 48;
    case GLSL_T_MAT4: return 64;
    default: return 4;
  }
}

// ---- GL Objects (in shareable namespace) -----------------------------------
struct gl_buf_obj {
  bool alive = false;
  u32 kind_hint = TRT_GL_BUF_VERTEX;
  std::vector<u8> data;
  u32 version = 0;
  bool dirty = false;
  bool mapped = false;
  u64 map_off = 0;
  u64 map_len = 0;
  u32 map_access = 0;
};

struct gl_tex_obj {
  bool alive = false;
  u32 target = TRT_GL_TARGET_2D;
  u32 fmt = TRT_GL_TEX_RGBA8;
  u32 w = 0, h = 0, depth = 1;
  u32 mip_levels = 1;
  u32 min_filter = GL_NEAREST_MIPMAP_LINEAR_V;
  u32 mag_filter = GL_LINEAR_V;
  u32 wrap_s = GL_REPEAT_V;
  u32 wrap_t = GL_REPEAT_V;
  u32 wrap_r = GL_REPEAT_V;
  u32 compare_mode = 0;
  std::vector<u8> level0_rgba;
  u32 version = 0;
};

struct gl_sampler_obj {
  bool alive = false;
  u32 min_filter = GL_NEAREST_MIPMAP_LINEAR_V;
  u32 mag_filter = GL_LINEAR_V;
  u32 wrap_s = GL_REPEAT_V;
  u32 wrap_t = GL_REPEAT_V;
  u32 compare_mode = 0;
};

struct gl_rbo_obj {
  bool alive = false;
  u32 backing_tex = 0; // backed directly by a shadow texture handle for FBO reuse
  u32 fmt = TRT_GL_TEX_RGBA8;
  u32 w = 0, h = 0;
};

struct gl_shader_obj {
  bool alive = false;
  u32 type = 0; // GL_VERTEX_SHADER / GL_FRAGMENT_SHADER / GL_COMPUTE_SHADER
  std::string source;
  std::string log;
  bool compiled = false;
  bool delete_flagged = false;
};

struct gl_prog_uniform {
  std::string name;
  glsl_val_kind kind = GLSL_T_UNKNOWN;
  u32 array_len = 1;
  int vs_off = -1;
  int ps_off = -1;
  int cs_off = -1;
  int vs_tex = -1;
  int ps_tex = -1;
  int cs_tex = -1;
  bool is_sampler = false;
  bool is_image = false;
  int sampler_unit = 0;
};

struct gl_prog_ubo {
  std::string name;
  u32 slot = 0;
  u32 size_bytes = 0;
  u32 block_binding = 0;
};

struct gl_prog_obj {
  bool alive = false;
  std::vector<u32> attached;
  std::map<std::string, int> explicit_attrib_locs;
  std::map<std::string, int> explicit_frag_locs;
  bool linked = false;
  bool delete_flagged = false;
  std::string log;
  glsl_compile_result vs_res;
  glsl_compile_result ps_res;
  glsl_compile_result cs_res;
  std::vector<u8> vs_cb0;
  std::vector<u8> ps_cb0;
  std::vector<u8> cs_cb0;
  std::vector<gl_prog_uniform> uniforms;
  std::map<std::string, u32> uniform_name_to_idx;
  std::vector<gl_prog_ubo> ubos;
  int mvp_uidx = -1, mv_uidx = -1, proj_uidx = -1, norm_uidx = -1;
};

struct gl_vao_obj {
  bool alive = false;
  u32 ibo = 0;
  trt_gl_attrib_binding attribs[16] = {};
  bool bound = false;
  const void* ptrs[16] = {};
};

struct gl_fbo_obj {
  bool alive = false;
  u32 color_tex[8] = {};
  u32 color_mip[8] = {};
  u32 color_layer[8] = {};
  u32 depth_tex = 0;
  u32 stencil_tex = 0;
  u32 draw_bufs[8] = {GL_COLOR_ATTACHMENT0_V};
  u32 draw_buf_count = 1;
  u32 read_buf = GL_COLOR_ATTACHMENT0_V;
  bool dirty = false;
};

struct gl_ff_vert {
  float pos[4];
  float nrm[3];
  float col[4];
  float tc0[4];
  float tc1[4];
};

enum gl_dl_op_kind : u8 {
  DL_OP_DRAW_IMMED = 1,
  DL_OP_LOAD_IDENTITY = 2,
  DL_OP_LOAD_MATRIX = 3,
  DL_OP_MULT_MATRIX = 4,
  DL_OP_PUSH_MATRIX = 5,
  DL_OP_POP_MATRIX = 6,
  DL_OP_TRANSLATE = 7,
  DL_OP_SCALE = 8,
  DL_OP_ROTATE = 9,
  DL_OP_BIND_TEX = 10,
  DL_OP_ENABLE = 11,
  DL_OP_DISABLE = 12,
};

struct gl_dl_op {
  gl_dl_op_kind kind{};
  u32 u = 0;
  u32 u2 = 0;
  float f[16] = {};
  std::vector<gl_ff_vert> verts;
};

struct gl_display_list {
  bool alive = false;
  std::vector<gl_dl_op> ops;
};

struct gl_share_group {
  u32 refs = 1;
  u32 next_tex = 1;
  u32 next_buf = 1;
  u32 next_sh = 1;
  u32 next_prog = 1;
  u32 next_samp = 1;
  u32 next_rbo = 1;
  u32 next_dl = 1;
  std::map<u32, gl_buf_obj> bufs;
  std::map<u32, gl_tex_obj> texs;
  std::map<u32, gl_sampler_obj> samps;
  std::map<u32, gl_rbo_obj> rbos;
  std::map<u32, gl_shader_obj> shaders;
  std::map<u32, gl_prog_obj> progs;
  std::map<u32, gl_prog_obj> ff_progs;
  std::map<u32, gl_display_list> dls;
};

struct wgl_ctx {
  bool alive = true;
  void* hdc = nullptr;
  gl_share_group* share = nullptr;
  u32 last_error = GL_NO_ERROR_V;

  // Per-context containers (VAOs & FBOs are non-shared per WGL spec)
  u32 next_vao = 1;
  u32 next_fbo = 1;
  u32 next_query = 1;
  std::map<u32, gl_vao_obj> vaos;
  std::map<u32, gl_fbo_obj> fbos;

  // Active bindings
  u32 cur_vao = 0;
  u32 cur_vbo = 0;
  u32 cur_ubo = 0;
  u32 cur_ssbo = 0;
  u32 cur_pbo_unpack = 0;
  u32 cur_prog = 0;
  u32 cur_draw_fbo = 0;
  u32 cur_read_fbo = 0;
  u32 cur_rbo = 0;
  u32 active_tex_unit = 0;
  u32 client_active_tex = 0;
  u32 bound_tex_2d[32] = {};
  u32 bound_tex_cube[32] = {};
  u32 bound_tex_3d[32] = {};
  u32 bound_tex_arr[32] = {};
  u32 bound_samplers[32] = {};
  u32 bound_image[8] = {};
  struct ubo_bind { u32 buf = 0; u32 off = 0; u32 sz = 0; };
  ubo_bind ubo_binds[16] = {};
  ubo_bind ssbo_binds[16] = {};

  // Render state
  float clear_color[4] = {0, 0, 0, 0};
  float clear_depth = 1.0f;
  int clear_stencil = 0;
  trt_gl_render_state rs{};
  bool tex2d_enabled[8] = {};
  bool lighting_en = false;
  bool alpha_test_en = false;
  u32 alpha_func = GL_ALWAYS_V;
  float alpha_ref = 0.0f;
  bool fog_en = false;
  u32 fog_mode = GL_EXP_V;
  float fog_density = 1.0f, fog_start = 0.0f, fog_end = 1.0f;
  float fog_color[4] = {0, 0, 0, 0};
  float light_ambient[4] = {0.2f, 0.2f, 0.2f, 1.0f};
  float light0_diffuse[4] = {1, 1, 1, 1};
  float light0_pos[4] = {0, 0, 1, 0};
  float light0_amb[4] = {0, 0, 0, 1};
  float mat_diffuse[4] = {0.8f, 0.8f, 0.8f, 1.0f};
  float mat_ambient[4] = {0.2f, 0.2f, 0.2f, 1.0f};
  int unpack_row_len = 0, unpack_skip_rows = 0, unpack_skip_pixels = 0, unpack_align = 4;
  int swap_interval = 1;

  // Matrix stacks
  u32 matrix_mode = GL_MODELVIEW_V;
  std::vector<gl_mat4> mv_stack = {mat4_identity()};
  std::vector<gl_mat4> proj_stack = {mat4_identity()};
  std::vector<gl_mat4> tex_stack[8];

  // Current vertex attributes & client arrays
  float cur_attrib[16][4] = {};
  struct client_arr {
    bool enabled = false;
    u32 comps = 4;
    u32 type = GL_FLOAT_V;
    u32 stride = 0;
    const void* ptr = nullptr;
    u32 vbo = 0;
  };
  client_arr ff_pos{};
  client_arr ff_nrm{};
  client_arr ff_col{};
  client_arr ff_tc[8]{};

  // Immediate mode & display list recording
  bool in_begin = false;
  u32 immed_mode = 0;
  std::vector<gl_ff_vert> immed_verts;
  u32 recording_list = 0;
  bool recording_exec = false;
  u32 list_base = 0;

  // Reusable per-draw scratch
  std::vector<u8> scratch_vb;
  std::vector<u32> scratch_idx;
  std::vector<u8> scratch_tex;
};

static std::vector<wgl_ctx*> g_contexts;
static std::map<void*, int> g_pixel_formats;
static std::string g_ext_string_cache;
static thread_local wgl_ctx* t_cur_ctx = nullptr;

static void init_ctx_defaults(wgl_ctx* c) {
  c->vaos[0].alive = true;
  c->fbos[0].alive = true;
  for (int i = 0; i < 8; ++i) c->tex_stack[i].push_back(mat4_identity());
  memset(&c->rs, 0, sizeof(c->rs));
  c->rs.depth_write = 1;
  c->rs.depth_func = GL_LESS_V;
  c->rs.cull_face = GL_BACK_V;
  c->rs.front_ccw = 1;
  c->rs.color_mask = 0x0Fu;
  c->rs.blend_src_rgb = GL_ONE_V;
  c->rs.blend_dst_rgb = GL_ZERO_V;
  c->rs.blend_src_a = GL_ONE_V;
  c->rs.blend_dst_a = GL_ZERO_V;
  c->rs.blend_eq_rgb = GL_FUNC_ADD_V;
  c->rs.blend_eq_a = GL_FUNC_ADD_V;
  c->rs.stencil_func = GL_ALWAYS_V;
  c->rs.stencil_mask = 0xFFFFFFFFu;
  c->rs.stencil_wmask = 0xFFFFFFFFu;
  c->rs.stencil_sfail = GL_KEEP_V;
  c->rs.stencil_dpfail = GL_KEEP_V;
  c->rs.stencil_dppass = GL_KEEP_V;
  c->rs.vp[0] = 0; c->rs.vp[1] = 0; c->rs.vp[2] = 1280; c->rs.vp[3] = 720;
  c->rs.sc[0] = 0; c->rs.sc[1] = 0; c->rs.sc[2] = 1280; c->rs.sc[3] = 720;
  c->rs.depth_min = 0.0f;
  c->rs.depth_max = 1.0f;
  for (int i = 0; i < 16; ++i) {
    c->cur_attrib[i][0] = 0.0f;
    c->cur_attrib[i][1] = 0.0f;
    c->cur_attrib[i][2] = 0.0f;
    c->cur_attrib[i][3] = 1.0f;
  }
  c->cur_attrib[2][2] = 1.0f; // normal default (0,0,1)
  c->cur_attrib[3][0] = c->cur_attrib[3][1] = c->cur_attrib[3][2] = c->cur_attrib[3][3] = 1.0f;
}

static wgl_ctx* cur_ctx() {
  return t_cur_ctx;
}

static void set_gl_err(wgl_ctx* c, u32 err) {
  if (c && c->last_error == GL_NO_ERROR_V) c->last_error = err;
}

static std::vector<gl_mat4>& active_mat_stack(wgl_ctx* c) {
  if (c->matrix_mode == GL_PROJECTION_V) return c->proj_stack;
  if (c->matrix_mode == GL_TEXTURE_V) {
    u32 u = std::min(c->active_tex_unit, 7u);
    return c->tex_stack[u];
  }
  return c->mv_stack;
}

static gl_vao_obj* get_cur_vao(wgl_ctx* c) {
  auto it = c->vaos.find(c->cur_vao);
  if (it != c->vaos.end() && it->second.alive) return &it->second;
  return &c->vaos[0];
}

static gl_buf_obj* get_target_buf(wgl_ctx* c, u32 target, u32* out_id = nullptr) {
  u32 bid = 0;
  if (target == GL_ARRAY_BUFFER_V) bid = c->cur_vbo;
  else if (target == GL_ELEMENT_ARRAY_BUFFER_V) bid = get_cur_vao(c)->ibo;
  else if (target == GL_UNIFORM_BUFFER_V) bid = c->cur_ubo;
  else if (target == GL_SHADER_STORAGE_BUFFER_V) bid = c->cur_ssbo;
  else if (target == GL_PIXEL_UNPACK_BUFFER_V) bid = c->cur_pbo_unpack;
  if (out_id) *out_id = bid;
  if (!bid) return nullptr;
  auto it = c->share->bufs.find(bid);
  return (it != c->share->bufs.end() && it->second.alive) ? &it->second : nullptr;
}

static u32 bound_tex_for_target(wgl_ctx* c, u32 target, u32 unit) {
  unit = std::min(unit, 31u);
  if (target == GL_TEXTURE_CUBE_MAP_V ||
      (target >= GL_TEXTURE_CUBE_MAP_POS_X && target <= GL_TEXTURE_CUBE_MAP_NEG_Z)) {
    return c->bound_tex_cube[unit];
  }
  if (target == GL_TEXTURE_3D_V) return c->bound_tex_3d[unit];
  if (target == GL_TEXTURE_2D_ARRAY_V) return c->bound_tex_arr[unit];
  return c->bound_tex_2d[unit];
}

static u32 gl_fmt_to_trt(u32 internal_fmt, u32 fmt, u32 type) {
  switch (internal_fmt) {
    case GL_COMPRESSED_RGB_S3TC_DXT1_V:
    case GL_COMPRESSED_RGBA_S3TC_DXT1_V: return TRT_GL_TEX_BC1;
    case GL_COMPRESSED_RGBA_S3TC_DXT3_V: return TRT_GL_TEX_BC2;
    case GL_COMPRESSED_RGBA_S3TC_DXT5_V: return TRT_GL_TEX_BC3;
    case GL_SRGB8_V:
    case GL_SRGB8_ALPHA8_V: return TRT_GL_TEX_SRGBA8;
    case GL_RGBA16F_V: return TRT_GL_TEX_RGBA16F;
    case GL_RGBA32F_V: return TRT_GL_TEX_RGBA32F;
    case GL_R32F_V: return TRT_GL_TEX_R32F;
    case GL_DEPTH24_STENCIL8_V:
    case GL_DEPTH_COMPONENT24_V:
    case GL_DEPTH_COMPONENT32F_V:
    case GL_DEPTH_COMPONENT_V:
    case GL_DEPTH_STENCIL_V: return TRT_GL_TEX_D24S8;
    default: break;
  }
  if (fmt == GL_DEPTH_COMPONENT_V || fmt == GL_DEPTH_STENCIL_V) return TRT_GL_TEX_D24S8;
  if (fmt == GL_BGRA_V && type == GL_UNSIGNED_BYTE_V) return TRT_GL_TEX_BGRA8;
  return TRT_GL_TEX_RGBA8;
}

static void sync_buf_to_host(u32 bid, gl_buf_obj& b) {
  if (!b.dirty) return;
  b.dirty = false;
  b.version++;
  if (g_gl_hooks_set && g_gl_hooks.sync_buffer) {
    g_gl_hooks.sync_buffer(bid, b.kind_hint,
                           b.data.empty() ? nullptr : b.data.data(),
                           (u32)b.data.size(), b.version, g_gl_hooks.user);
  }
}

static void convert_pixels_to_rgba8(const u8* src, int w, int h, u32 fmt, u32 type,
                                    int row_len, int skip_rows, int skip_px,
                                    int align, std::vector<u8>& dst) {
  dst.assign((size_t)w * (size_t)h * 4u, 255u);
  if (!src) {
    memset(dst.data(), 0, dst.size());
    return;
  }
  u32 src_comps = 4;
  switch (fmt) {
    case GL_RED_V:
    case GL_LUMINANCE_V: src_comps = 1; break;
    case GL_RG_V:
    case GL_LUMINANCE_ALPHA_V: src_comps = 2; break;
    case GL_RGB_V:
    case GL_BGR_V: src_comps = 3; break;
    case GL_RGBA_V:
    case GL_BGRA_V: src_comps = 4; break;
    default: src_comps = 4; break;
  }
  u32 elem_bytes = src_comps;
  if (type == GL_UNSIGNED_SHORT_5_6_5_V || type == GL_UNSIGNED_SHORT_4_4_4_4_V ||
      type == GL_UNSIGNED_SHORT_5_5_5_1_V) {
    elem_bytes = 2;
  } else if (type == GL_FLOAT_V) {
    elem_bytes = src_comps * 4u;
  } else if (type == GL_UNSIGNED_SHORT_V) {
    elem_bytes = src_comps * 2u;
  }
  const u32 rpx = (row_len > 0) ? (u32)row_len : (u32)w;
  const u32 raw_pitch = rpx * elem_bytes;
  const u32 a = (align > 1) ? (u32)align : 1u;
  const u32 pitch = ((raw_pitch + a - 1u) / a) * a;

  for (int y = 0; y < h; ++y) {
    const u8* row = src + ((size_t)(y + skip_rows) * pitch) +
                    (size_t)skip_px * elem_bytes;
    u8* out = dst.data() + (size_t)y * (size_t)w * 4u;
    for (int x = 0; x < w; ++x) {
      const u8* p = row + (size_t)x * elem_bytes;
      u8 r = 0, g = 0, b = 0, al = 255;
      if (type == GL_UNSIGNED_SHORT_5_6_5_V) {
        u16 v = (u16)(p[0] | (p[1] << 8));
        r = (u8)(((v >> 11) & 0x1Fu) * 255u / 31u);
        g = (u8)(((v >> 5) & 0x3Fu) * 255u / 63u);
        b = (u8)((v & 0x1Fu) * 255u / 31u);
      } else if (type == GL_UNSIGNED_SHORT_4_4_4_4_V) {
        u16 v = (u16)(p[0] | (p[1] << 8));
        r = (u8)(((v >> 12) & 0xFu) * 17u);
        g = (u8)(((v >> 8) & 0xFu) * 17u);
        b = (u8)(((v >> 4) & 0xFu) * 17u);
        al = (u8)((v & 0xFu) * 17u);
      } else if (type == GL_UNSIGNED_SHORT_5_5_5_1_V) {
        u16 v = (u16)(p[0] | (p[1] << 8));
        r = (u8)(((v >> 11) & 0x1Fu) * 255u / 31u);
        g = (u8)(((v >> 6) & 0x1Fu) * 255u / 31u);
        b = (u8)(((v >> 1) & 0x1Fu) * 255u / 31u);
        al = (v & 1u) ? 255u : 0u;
      } else if (type == GL_FLOAT_V) {
        const float* fp = (const float*)p;
        auto cl = [](float v) -> u8 {
          if (v <= 0.0f) return 0;
          if (v >= 1.0f) return 255;
          return (u8)(v * 255.0f + 0.5f);
        };
        if (fmt == GL_RED_V || fmt == GL_LUMINANCE_V) {
          r = g = b = cl(fp[0]);
        } else if (fmt == GL_RG_V) {
          r = cl(fp[0]); g = cl(fp[1]);
        } else if (fmt == GL_LUMINANCE_ALPHA_V) {
          r = g = b = cl(fp[0]); al = cl(fp[1]);
        } else if (fmt == GL_RGB_V) {
          r = cl(fp[0]); g = cl(fp[1]); b = cl(fp[2]);
        } else if (fmt == GL_BGR_V) {
          b = cl(fp[0]); g = cl(fp[1]); r = cl(fp[2]);
        } else if (fmt == GL_BGRA_V) {
          b = cl(fp[0]); g = cl(fp[1]); r = cl(fp[2]); al = cl(fp[3]);
        } else {
          r = cl(fp[0]); g = cl(fp[1]); b = cl(fp[2]); al = cl(fp[3]);
        }
      } else {
        // GL_UNSIGNED_BYTE / generic
        if (fmt == GL_RED_V) {
          r = p[0]; g = 0; b = 0; al = 255;
        } else if (fmt == GL_LUMINANCE_V) {
          r = g = b = p[0]; al = 255;
        } else if (fmt == GL_RG_V) {
          r = p[0]; g = p[1]; b = 0; al = 255;
        } else if (fmt == GL_LUMINANCE_ALPHA_V) {
          r = g = b = p[0]; al = p[1];
        } else if (fmt == GL_RGB_V) {
          r = p[0]; g = p[1]; b = p[2]; al = 255;
        } else if (fmt == GL_BGR_V) {
          b = p[0]; g = p[1]; r = p[2]; al = 255;
        } else if (fmt == GL_BGRA_V) {
          b = p[0]; g = p[1]; r = p[2]; al = p[3];
        } else {
          r = p[0]; g = p[1]; b = p[2]; al = p[3];
        }
      }
      out[x * 4 + 0] = r;
      out[x * 4 + 1] = g;
      out[x * 4 + 2] = b;
      out[x * 4 + 3] = al;
    }
  }
}

// ---- Program uniform reflection & linking ----------------------------------
static void populate_program_reflection(gl_prog_obj& p) {
  p.uniforms.clear();
  p.uniform_name_to_idx.clear();
  p.ubos.clear();
  p.vs_cb0.assign(std::max(p.vs_res.cb0_bytes, 16u), 0);
  p.ps_cb0.assign(std::max(p.ps_res.cb0_bytes, 16u), 0);
  p.cs_cb0.assign(std::max(p.cs_res.cb0_bytes, 16u), 0);
  p.mvp_uidx = p.mv_uidx = p.proj_uidx = p.norm_uidx = -1;

  auto merge_stage = [&](const glsl_compile_result& res, int stage_kind) {
    for (const auto& u : res.uniforms) {
      auto it = p.uniform_name_to_idx.find(u.name);
      u32 idx = 0;
      if (it == p.uniform_name_to_idx.end()) {
        idx = (u32)p.uniforms.size();
        gl_prog_uniform pu;
        pu.name = u.name;
        pu.kind = u.kind;
        pu.array_len = u.array_len;
        pu.is_sampler = (u.kind >= GLSL_T_SAMPLER_2D && u.kind <= GLSL_T_SAMPLER_2D_ARRAY);
        pu.is_image = (u.kind == GLSL_T_IMAGE_2D);
        pu.sampler_unit = (u.tex_slot >= 0) ? u.tex_slot : 0;
        p.uniforms.push_back(std::move(pu));
        p.uniform_name_to_idx[u.name] = idx;
      } else {
        idx = it->second;
      }
      gl_prog_uniform& pu = p.uniforms[idx];
      if (pu.is_sampler || pu.is_image) {
        if (stage_kind == 0) pu.vs_tex = u.tex_slot;
        else if (stage_kind == 1) pu.ps_tex = u.tex_slot;
        else pu.cs_tex = u.tex_slot;
      } else {
        if (stage_kind == 0) pu.vs_off = (int)u.cb_offset;
        else if (stage_kind == 1) pu.ps_off = (int)u.cb_offset;
        else pu.cs_off = (int)u.cb_offset;
      }
    }
    for (const auto& ub : res.ubos) {
      bool found = false;
      for (const auto& ex : p.ubos) {
        if (ex.name == ub.block_name) { found = true; break; }
      }
      if (!found) {
        p.ubos.push_back({ub.block_name, ub.binding, ub.size_bytes, ub.binding});
      }
    }
  };
  if (p.vs_res.ok) merge_stage(p.vs_res, 0);
  if (p.ps_res.ok) merge_stage(p.ps_res, 1);
  if (p.cs_res.ok) merge_stage(p.cs_res, 2);

  auto find_u = [&](const char* nm) -> int {
    auto it = p.uniform_name_to_idx.find(nm);
    return (it == p.uniform_name_to_idx.end()) ? -1 : (int)it->second;
  };
  p.mvp_uidx = find_u("gl_ModelViewProjectionMatrix");
  p.mv_uidx = find_u("gl_ModelViewMatrix");
  p.proj_uidx = find_u("gl_ProjectionMatrix");
  p.norm_uidx = find_u("gl_NormalMatrix");
}

static void write_prog_cb_bytes(gl_prog_obj& p, u32 uidx, u32 elem,
                                const void* data, u32 bytes) {
  if (uidx >= p.uniforms.size()) return;
  const gl_prog_uniform& u = p.uniforms[uidx];
  const u32 elem_sz = hlsl_val_kind_comp_bytes(u.kind);
  const u32 stride = (elem_sz <= 16) ? 16u : elem_sz;
  const u32 rel = elem * stride;
  auto patch = [&](std::vector<u8>& cb, int base) {
    if (base < 0) return;
    u32 off = (u32)base + rel;
    if (off >= cb.size()) return;
    u32 cp = std::min(bytes, (u32)cb.size() - off);
    memcpy(cb.data() + off, data, cp);
  };
  patch(p.vs_cb0, u.vs_off);
  patch(p.ps_cb0, u.ps_off);
  patch(p.cs_cb0, u.cs_off);
}

static gl_prog_obj* ensure_ff_program(wgl_ctx* c) {
  const u32 key_bits =
      (c->tex2d_enabled[0] ? 1u : 0u) |
      (c->tex2d_enabled[1] ? 2u : 0u) |
      (c->lighting_en ? 4u : 0u) |
      (c->alpha_test_en ? 8u : 0u) |
      (c->fog_en ? 16u : 0u);
  const u32 prog_id = GL_FF_PROG_BASE | key_bits;
  auto it = c->share->ff_progs.find(prog_id);
  if (it != c->share->ff_progs.end() && it->second.linked) {
    return &it->second;
  }
  gl_prog_obj& p = c->share->ff_progs[prog_id];
  p.alive = true;

  std::string vs = "#version 120\n"
      "attribute vec4 aPos;\n"
      "uniform mat4 u_mvp;\n"
      "uniform mat4 u_mv;\n"
      "uniform mat4 u_tex0;\n"
      "varying vec4 v_col;\n"
      "varying vec2 v_tc0;\n"
      "varying vec2 v_tc1;\n"
      "varying float v_fog_z;\n";
  vs += "varying vec4 v_nrm;\n";
  vs += "void main() {\n"
        "  vec4 vp = u_mv * gl_Vertex;\n"
        "  gl_Position = u_mvp * gl_Vertex;\n"
        "  v_col = gl_Color;\n"
        "  v_tc0 = (u_tex0 * gl_MultiTexCoord0).xy;\n"
        "  v_tc1 = gl_MultiTexCoord1.xy;\n"
        "  v_fog_z = abs(vp.z);\n"
        "  v_nrm = vec4(normalize(gl_Normal), 0.0);\n"
        "}\n";
  // Notice aPos wasn't needed when gl_Vertex is referenced; drop unused aPos:
  const size_t apos_pos = vs.find("attribute vec4 aPos;\n");
  if (apos_pos != std::string::npos) vs.erase(apos_pos, 20);

  std::string ps = "#version 120\n"
      "uniform vec4 u_light_dir;\n"
      "uniform vec4 u_light_diff;\n"
      "uniform vec4 u_light_amb;\n"
      "uniform vec4 u_fog_col;\n"
      "uniform vec4 u_fog_params;\n" // x=mode(0=none,1=lin,2=exp,3=exp2), y=density, z=start, w=end
      "uniform float u_alpha_ref;\n"
      "uniform int u_alpha_func;\n"
      "uniform sampler2D u_tex0;\n"
      "uniform sampler2D u_tex1;\n"
      "varying vec4 v_col;\n"
      "varying vec2 v_tc0;\n"
      "varying vec2 v_tc1;\n"
      "varying float v_fog_z;\n"
      "varying vec4 v_nrm;\n"
      "void main() {\n"
      "  vec4 col = v_col;\n";
  if (c->lighting_en) {
    ps += "  float ndl = max(dot(normalize(v_nrm.xyz), normalize(u_light_dir.xyz)), 0.0);\n"
          "  col.rgb *= (u_light_amb.rgb + u_light_diff.rgb * ndl);\n";
  }
  if (c->tex2d_enabled[0]) ps += "  col *= texture2D(u_tex0, v_tc0);\n";
  if (c->tex2d_enabled[1]) ps += "  col *= texture2D(u_tex1, v_tc1);\n";
  if (c->alpha_test_en) {
    ps += "  if (u_alpha_func == 516 && !(col.a > u_alpha_ref)) discard;\n"
          "  if (u_alpha_func == 518 && !(col.a >= u_alpha_ref)) discard;\n"
          "  if (u_alpha_func == 513 && !(col.a < u_alpha_ref)) discard;\n"
          "  if (u_alpha_func == 515 && !(col.a <= u_alpha_ref)) discard;\n"
          "  if (u_alpha_func == 514 && !(col.a == u_alpha_ref)) discard;\n"
          "  if (u_alpha_func == 517 && !(col.a != u_alpha_ref)) discard;\n"
          "  if (u_alpha_func == 512) discard;\n";
  }
  if (c->fog_en) {
    ps += "  float ff = 1.0;\n"
          "  if (u_fog_params.x < 1.5) ff = clamp((u_fog_params.w - v_fog_z) / max(u_fog_params.w - u_fog_params.z, 0.0001), 0.0, 1.0);\n"
          "  else if (u_fog_params.x < 2.5) ff = clamp(exp(-u_fog_params.y * v_fog_z), 0.0, 1.0);\n"
          "  else { float e = u_fog_params.y * v_fog_z; ff = clamp(exp(-e * e), 0.0, 1.0); }\n"
          "  col.rgb = mix(u_fog_col.rgb, col.rgb, ff);\n";
  }
  ps += "  gl_FragColor = col;\n}\n";

  glsl_link_bindings link{};
  p.vs_res = glsl_to_hlsl(vs.c_str(), (u32)vs.size(), GLSL_STAGE_VERTEX, &link);
  for (const auto& ov : p.vs_res.outputs) {
    if (!ov.is_builtin || ov.name.find("gl_") == std::string::npos) {
      link.varying_locs.push_back({ov.name.c_str(), ov.location});
    }
  }
  p.ps_res = glsl_to_hlsl(ps.c_str(), (u32)ps.size(), GLSL_STAGE_FRAGMENT, &link);
  populate_program_reflection(p);
  for (auto& u : p.uniforms) {
    if (u.name == "u_tex0") u.sampler_unit = 0;
    else if (u.name == "u_tex1") u.sampler_unit = 1;
  }
  p.linked = (p.vs_res.ok && p.ps_res.ok);
  if (p.linked && g_gl_hooks_set && g_gl_hooks.compile_program) {
    g_gl_hooks.compile_program(
        prog_id, p.vs_res.hlsl.c_str(), (u32)p.vs_res.hlsl.size(),
        p.ps_res.hlsl.c_str(), (u32)p.ps_res.hlsl.size(), nullptr, 0,
        g_gl_hooks.user);
  }
  return &p;
}

static void push_ff_and_compat_uniforms(wgl_ctx* c, gl_prog_obj* p) {
  const gl_mat4& mv = c->mv_stack.back();
  const gl_mat4& pr = c->proj_stack.back();
  const gl_mat4 mvp = mat4_mul(pr, mv);
  if (p->mvp_uidx >= 0) write_prog_cb_bytes(p[0], (u32)p->mvp_uidx, 0, mvp.m, 64);
  if (p->mv_uidx >= 0) write_prog_cb_bytes(p[0], (u32)p->mv_uidx, 0, mv.m, 64);
  if (p->proj_uidx >= 0) write_prog_cb_bytes(p[0], (u32)p->proj_uidx, 0, pr.m, 64);
  if (p->norm_uidx >= 0) {
    float n3[12];
    mat4_normal3x3(mv, n3);
    write_prog_cb_bytes(p[0], (u32)p->norm_uidx, 0, n3, 48);
  }
  auto set_nm = [&](const char* nm, const void* data, u32 bytes) {
    auto it = p->uniform_name_to_idx.find(nm);
    if (it != p->uniform_name_to_idx.end()) {
      write_prog_cb_bytes(*p, it->second, 0, data, bytes);
    }
  };
  set_nm("u_mvp", mvp.m, 64);
  set_nm("u_mv", mv.m, 64);
  set_nm("u_tex0", c->tex_stack[0].back().m, 64);
  if (c->lighting_en) {
    float amb[4] = {
        c->light_ambient[0] * c->mat_ambient[0] + c->light0_amb[0] * c->mat_ambient[0],
        c->light_ambient[1] * c->mat_ambient[1] + c->light0_amb[1] * c->mat_ambient[1],
        c->light_ambient[2] * c->mat_ambient[2] + c->light0_amb[2] * c->mat_ambient[2],
        1.0f};
    float diff[4] = {
        c->light0_diffuse[0] * c->mat_diffuse[0],
        c->light0_diffuse[1] * c->mat_diffuse[1],
        c->light0_diffuse[2] * c->mat_diffuse[2],
        c->mat_diffuse[3]};
    set_nm("u_light_dir", c->light0_pos, 16);
    set_nm("u_light_diff", diff, 16);
    set_nm("u_light_amb", amb, 16);
  }
  if (c->alpha_test_en) {
    int af = (int)c->alpha_func;
    set_nm("u_alpha_ref", &c->alpha_ref, 4);
    set_nm("u_alpha_func", &af, 4);
  }
  if (c->fog_en) {
    float fm = (c->fog_mode == GL_LINEAR_FOG_V) ? 1.0f
             : (c->fog_mode == GL_EXP_V ? 2.0f : 3.0f);
    float fp[4] = {fm, c->fog_density, c->fog_start, c->fog_end};
    set_nm("u_fog_col", c->fog_color, 16);
    set_nm("u_fog_params", fp, 16);
  }
}

static void fill_sampler_and_ubo_state(wgl_ctx* c, const gl_prog_obj& p,
                                       trt_gl_draw_cmd& cmd) {
  cmd.vs_tex_count = 0;
  cmd.ps_tex_count = 0;
  for (const auto& u : p.uniforms) {
    if (!u.is_sampler) continue;
    u32 unit = (u32)std::max(0, std::min(31, u.sampler_unit));
    u32 tex_id = c->bound_tex_2d[unit];
    if (!tex_id) tex_id = c->bound_tex_cube[unit];
    if (!tex_id) tex_id = c->bound_tex_arr[unit];
    if (!tex_id) tex_id = c->bound_tex_3d[unit];
    trt_gl_sampler_slot slot{};
    slot.gl_tex = tex_id;
    slot.min_filter = GL_LINEAR_V;
    slot.mag_filter = GL_LINEAR_V;
    slot.wrap_s = GL_REPEAT_V;
    slot.wrap_t = GL_REPEAT_V;
    if (tex_id) {
      auto tit = c->share->texs.find(tex_id);
      if (tit != c->share->texs.end()) {
        slot.min_filter = tit->second.min_filter;
        slot.mag_filter = tit->second.mag_filter;
        slot.wrap_s = tit->second.wrap_s;
        slot.wrap_t = tit->second.wrap_t;
        slot.compare_mode = tit->second.compare_mode;
      }
    }
    u32 sid = c->bound_samplers[unit];
    if (sid) {
      auto sit = c->share->samps.find(sid);
      if (sit != c->share->samps.end()) {
        slot.min_filter = sit->second.min_filter;
        slot.mag_filter = sit->second.mag_filter;
        slot.wrap_s = sit->second.wrap_s;
        slot.wrap_t = sit->second.wrap_t;
        slot.compare_mode = sit->second.compare_mode;
      }
    }
    if (u.vs_tex >= 0 && u.vs_tex < 16) {
      cmd.vs_tex[u.vs_tex] = slot;
      cmd.vs_tex_count = std::max(cmd.vs_tex_count, (u32)(u.vs_tex + 1));
    }
    if (u.ps_tex >= 0 && u.ps_tex < 16) {
      cmd.ps_tex[u.ps_tex] = slot;
      cmd.ps_tex_count = std::max(cmd.ps_tex_count, (u32)(u.ps_tex + 1));
    }
  }
  cmd.ubo_count = 0;
  for (const auto& ub : p.ubos) {
    if (ub.slot >= 14 || ub.block_binding >= 16) continue;
    const auto& b = c->ubo_binds[ub.block_binding];
    if (b.buf) {
      auto bit = c->share->bufs.find(b.buf);
      if (bit != c->share->bufs.end()) {
        bit->second.kind_hint = TRT_GL_BUF_UNIFORM;
        sync_buf_to_host(b.buf, bit->second);
      }
      cmd.ubos[ub.slot] = {b.buf, b.off,
                           b.sz ? b.sz : (bit != c->share->bufs.end() ? (u32)bit->second.data.size() : 0u)};
      cmd.ubo_count = std::max(cmd.ubo_count, ub.slot + 1u);
    }
  }
}

static void sync_active_fbo(wgl_ctx* c) {
  if (!c->cur_draw_fbo) return;
  auto it = c->fbos.find(c->cur_draw_fbo);
  if (it == c->fbos.end() || !it->second.alive || !it->second.dirty) return;
  it->second.dirty = false;
  if (g_gl_hooks_set && g_gl_hooks.sync_fbo) {
    g_gl_hooks.sync_fbo(c->cur_draw_fbo, it->second.color_tex, it->second.color_mip,
                        it->second.color_layer, it->second.draw_buf_count,
                        it->second.depth_tex, it->second.stencil_tex, g_gl_hooks.user);
  }
}

static u32 convert_nonindex_prim(u32 mode, u32 first, u32 count,
                                 std::vector<u32>& out_idx, u32& out_prim) {
  out_idx.clear();
  if (mode == GL_POINTS_V) { out_prim = TRT_GL_PRIM_POINTS; return count; }
  if (mode == GL_LINES_V) { out_prim = TRT_GL_PRIM_LINES; return count; }
  if (mode == GL_LINE_STRIP_V) { out_prim = TRT_GL_PRIM_LINE_STRIP; return count; }
  if (mode == GL_TRIANGLES_V) { out_prim = TRT_GL_PRIM_TRIANGLES; return count; }
  if (mode == GL_TRIANGLE_STRIP_V) { out_prim = TRT_GL_PRIM_TRIANGLE_STRIP; return count; }
  if (mode == GL_LINE_LOOP_V && count >= 2) {
    out_prim = TRT_GL_PRIM_LINE_STRIP;
    for (u32 i = 0; i < count; ++i) out_idx.push_back(first + i);
    out_idx.push_back(first);
    return (u32)out_idx.size();
  }
  if ((mode == GL_TRIANGLE_FAN_V || mode == GL_POLYGON_V) && count >= 3) {
    out_prim = TRT_GL_PRIM_TRIANGLES;
    for (u32 i = 1; i + 1 < count; ++i) {
      out_idx.push_back(first);
      out_idx.push_back(first + i);
      out_idx.push_back(first + i + 1);
    }
    return (u32)out_idx.size();
  }
  if (mode == GL_QUADS_V && count >= 4) {
    out_prim = TRT_GL_PRIM_TRIANGLES;
    for (u32 q = 0; q + 3 < count; q += 4) {
      u32 v0 = first + q, v1 = v0 + 1, v2 = v0 + 2, v3 = v0 + 3;
      out_idx.push_back(v0); out_idx.push_back(v1); out_idx.push_back(v2);
      out_idx.push_back(v0); out_idx.push_back(v2); out_idx.push_back(v3);
    }
    return (u32)out_idx.size();
  }
  if (mode == GL_QUAD_STRIP_V && count >= 4) {
    out_prim = TRT_GL_PRIM_TRIANGLES;
    for (u32 q = 0; q + 3 < count; q += 2) {
      u32 a = first + q, b = a + 1, c2 = a + 2, d = a + 3;
      out_idx.push_back(a); out_idx.push_back(b); out_idx.push_back(d);
      out_idx.push_back(a); out_idx.push_back(d); out_idx.push_back(c2);
    }
    return (u32)out_idx.size();
  }
  out_prim = TRT_GL_PRIM_TRIANGLES;
  return count;
}

static void convert_indexed_prim(u32 mode, const u8* idx_ptr, u32 idx_type,
                                 u32 count, std::vector<u32>& out_idx,
                                 u32& out_prim) {
  auto read_i = [&](u32 i) -> u32 {
    if (idx_type == GL_UNSIGNED_BYTE_V) return idx_ptr[i];
    if (idx_type == GL_UNSIGNED_SHORT_V) return ((const u16*)idx_ptr)[i];
    return ((const u32*)idx_ptr)[i];
  };
  out_idx.clear();
  if (mode == GL_TRIANGLES_V || mode == GL_TRIANGLE_STRIP_V ||
      mode == GL_LINES_V || mode == GL_LINE_STRIP_V || mode == GL_POINTS_V) {
    out_prim = mode;
    if (idx_type == GL_UNSIGNED_BYTE_V) {
      out_idx.reserve(count);
      for (u32 i = 0; i < count; ++i) out_idx.push_back(idx_ptr[i]);
    }
    return;
  }
  if ((mode == GL_TRIANGLE_FAN_V || mode == GL_POLYGON_V) && count >= 3) {
    out_prim = TRT_GL_PRIM_TRIANGLES;
    u32 v0 = read_i(0);
    for (u32 i = 1; i + 1 < count; ++i) {
      out_idx.push_back(v0); out_idx.push_back(read_i(i)); out_idx.push_back(read_i(i + 1));
    }
    return;
  }
  if (mode == GL_QUADS_V && count >= 4) {
    out_prim = TRT_GL_PRIM_TRIANGLES;
    for (u32 q = 0; q + 3 < count; q += 4) {
      u32 v0 = read_i(q), v1 = read_i(q + 1), v2 = read_i(q + 2), v3 = read_i(q + 3);
      out_idx.push_back(v0); out_idx.push_back(v1); out_idx.push_back(v2);
      out_idx.push_back(v0); out_idx.push_back(v2); out_idx.push_back(v3);
    }
    return;
  }
  if (mode == GL_LINE_LOOP_V && count >= 2) {
    out_prim = TRT_GL_PRIM_LINE_STRIP;
    for (u32 i = 0; i < count; ++i) out_idx.push_back(read_i(i));
    out_idx.push_back(read_i(0));
    return;
  }
  out_prim = TRT_GL_PRIM_TRIANGLES;
}

static void emit_immediate_verts(wgl_ctx* c, u32 mode,
                                 const std::vector<gl_ff_vert>& verts) {
  if (verts.empty()) return;
  gl_prog_obj* prog = nullptr;
  u32 prog_id = c->cur_prog;
  if (prog_id) {
    auto it = c->share->progs.find(prog_id);
    if (it != c->share->progs.end() && it->second.linked) prog = &it->second;
  }
  if (!prog) {
    prog = ensure_ff_program(c);
    prog_id = prog ? (GL_FF_PROG_BASE | (c->tex2d_enabled[0] ? 1u : 0u) |
                      (c->tex2d_enabled[1] ? 2u : 0u) |
                      (c->lighting_en ? 4u : 0u) |
                      (c->alpha_test_en ? 8u : 0u) |
                      (c->fog_en ? 16u : 0u)) : 0;
  }
  if (!prog) return;
  push_ff_and_compat_uniforms(c, prog);
  sync_active_fbo(c);

  u32 prim = TRT_GL_PRIM_TRIANGLES;
  u32 dcount = convert_nonindex_prim(mode, 0, (u32)verts.size(), c->scratch_idx, prim);

  trt_gl_attrib_binding ab[5] = {};
  ab[0] = {0, 0xFFFFFFFFu, 4, GL_FLOAT_V, 0, 0, 0, 1, (u32)sizeof(gl_ff_vert), 0};
  ab[1] = {2, 0xFFFFFFFFu, 3, GL_FLOAT_V, 0, 0, 0, 1, (u32)sizeof(gl_ff_vert), 16};
  ab[2] = {3, 0xFFFFFFFFu, 4, GL_FLOAT_V, 0, 0, 0, 1, (u32)sizeof(gl_ff_vert), 28};
  ab[3] = {8, 0xFFFFFFFFu, 4, GL_FLOAT_V, 0, 0, 0, 1, (u32)sizeof(gl_ff_vert), 44};
  ab[4] = {9, 0xFFFFFFFFu, 4, GL_FLOAT_V, 0, 0, 0, 1, (u32)sizeof(gl_ff_vert), 60};

  trt_gl_draw_cmd cmd{};
  cmd.gl_prog = prog_id;
  cmd.prim_mode = prim;
  cmd.indexed = c->scratch_idx.empty() ? 0 : 1;
  cmd.use_inline_idx = cmd.indexed;
  cmd.idx_type = GL_UNSIGNED_INT_V;
  cmd.inline_idx = c->scratch_idx.empty() ? nullptr : c->scratch_idx.data();
  cmd.inline_idx_bytes = (u32)(c->scratch_idx.size() * 4u);
  cmd.count = cmd.indexed ? (u32)c->scratch_idx.size() : dcount;
  cmd.first = 0;
  cmd.instance_count = 1;
  cmd.inline_vb = verts.data();
  cmd.inline_vb_bytes = (u32)(verts.size() * sizeof(gl_ff_vert));
  cmd.attribs = ab;
  cmd.attrib_count = 5;
  cmd.active_fbo = c->cur_draw_fbo;
  cmd.vs_cb0 = prog->vs_cb0.data();
  cmd.vs_cb0_bytes = (u32)prog->vs_cb0.size();
  cmd.ps_cb0 = prog->ps_cb0.data();
  cmd.ps_cb0_bytes = (u32)prog->ps_cb0.size();
  fill_sampler_and_ubo_state(c, *prog, cmd);
  cmd.rs = c->rs;
  if (g_gl_hooks_set && g_gl_hooks.draw) g_gl_hooks.draw(&cmd, g_gl_hooks.user);
}

static void emit_array_draw(wgl_ctx* c, u32 mode, u8 indexed, u32 count,
                            u32 first, u32 idx_type, const void* idx_ptr_or_off,
                            u32 instance_count, int base_vertex) {
  if (!count || !instance_count) return;
  gl_prog_obj* prog = nullptr;
  u32 prog_id = c->cur_prog;
  if (prog_id) {
    auto it = c->share->progs.find(prog_id);
    if (it != c->share->progs.end() && it->second.linked) prog = &it->second;
  }
  const bool ff = (prog == nullptr);
  if (ff) {
    prog = ensure_ff_program(c);
    prog_id = GL_FF_PROG_BASE | (c->tex2d_enabled[0] ? 1u : 0u) |
              (c->tex2d_enabled[1] ? 2u : 0u) |
              (c->lighting_en ? 4u : 0u) |
              (c->alpha_test_en ? 8u : 0u) |
              (c->fog_en ? 16u : 0u);
  }
  if (!prog) return;
  push_ff_and_compat_uniforms(c, prog);
  sync_active_fbo(c);

  gl_vao_obj* vao = get_cur_vao(c);
  std::vector<trt_gl_attrib_binding> active_ab;
  const void* inline_vb_ptr = nullptr;
  u32 inline_vb_bytes = 0;

  if (ff) {
    // Fixed-function client arrays OR vertex arrays
    auto add_ff = [&](const wgl_ctx::client_arr& a, u32 loc) {
      if (!a.enabled) return;
      trt_gl_attrib_binding b{};
      b.location = loc;
      b.vbo_gl = a.vbo ? a.vbo : 0xFFFFFFFFu;
      b.components = a.comps;
      b.gl_type = a.type;
      b.normalized = (loc == 2 || loc == 3) ? 1 : 0;
      b.enabled = 1;
      b.stride = a.stride ? a.stride : (a.comps * gl_type_bytes(a.type));
      b.offset = (u32)(uintptr_t)a.ptr;
      active_ab.push_back(b);
      if (a.vbo) {
        auto bit = c->share->bufs.find(a.vbo);
        if (bit != c->share->bufs.end()) sync_buf_to_host(a.vbo, bit->second);
      }
    };
    add_ff(c->ff_pos, 0);
    add_ff(c->ff_nrm, 2);
    add_ff(c->ff_col, 3);
    for (int t = 0; t < 2; ++t) add_ff(c->ff_tc[t], 8u + (u32)t);
  } else {
    for (u32 i = 0; i < 16; ++i) {
      const auto& ab = vao->attribs[i];
      if (!ab.enabled) continue;
      trt_gl_attrib_binding copy = ab;
      if (ab.vbo_gl != 0 && ab.vbo_gl != 0xFFFFFFFFu) {
        auto bit = c->share->bufs.find(ab.vbo_gl);
        if (bit != c->share->bufs.end()) sync_buf_to_host(ab.vbo_gl, bit->second);
      } else {
        copy.vbo_gl = 0xFFFFFFFFu;
      }
      active_ab.push_back(copy);
    }
  }

  // Gather client pointers (vbo_gl == 0xFFFFFFFFu) into c->scratch_vb
  bool has_client_arr = false;
  u32 max_vtx = first + count;
  if (indexed && max_vtx < 65536u) max_vtx = std::max(max_vtx, count);
  c->scratch_vb.clear();
  for (auto& ab : active_ab) {
    if (ab.vbo_gl != 0xFFFFFFFFu) continue;
    const u8* ptr = ff ? nullptr : (const u8*)vao->ptrs[ab.location];
    if (ff) {
      if (ab.location == 0) ptr = (const u8*)c->ff_pos.ptr;
      else if (ab.location == 2) ptr = (const u8*)c->ff_nrm.ptr;
      else if (ab.location == 3) ptr = (const u8*)c->ff_col.ptr;
      else if (ab.location == 8) ptr = (const u8*)c->ff_tc[0].ptr;
      else if (ab.location == 9) ptr = (const u8*)c->ff_tc[1].ptr;
    }
    if (!ptr) continue;
    has_client_arr = true;
    u32 bytes = max_vtx * ab.stride;
    if (!bytes) bytes = ab.stride;
    u32 base_off = (u32)((c->scratch_vb.size() + 15u) & ~15u);
    c->scratch_vb.resize(base_off + bytes);
    memcpy(c->scratch_vb.data() + base_off, ptr, bytes);
    ab.offset = base_off;
  }
  if (has_client_arr) {
    inline_vb_ptr = c->scratch_vb.data();
    inline_vb_bytes = (u32)c->scratch_vb.size();
  }

  // Fallback constant row for any disabled attrib slot (packed at end or via default_attribs)
  // Emit a 16-entry constant vertex block at index 0 of scratch_vb? Even better:
  // add missing required VS input locations with a 1-vertex constant entry in scratch_vb!
  for (const auto& iv : prog->vs_res.inputs) {
    if (iv.location < 0 || iv.location >= 16) continue;
    bool present = false;
    for (const auto& ab : active_ab) if (ab.location == (u32)iv.location) { present = true; break; }
    if (!present) {
      u32 base_off = (u32)((c->scratch_vb.size() + 15u) & ~15u);
      c->scratch_vb.resize(base_off + 16u);
      memcpy(c->scratch_vb.data() + base_off, c->cur_attrib[iv.location], 16u);
      trt_gl_attrib_binding db{};
      db.location = (u32)iv.location;
      db.vbo_gl = 0xFFFFFFFFu;
      db.components = 4;
      db.gl_type = GL_FLOAT_V;
      db.enabled = 1;
      db.divisor = 0xFEu; // special marker for gl_backend: constant per-instance step 0
      db.stride = 16;
      db.offset = base_off;
      active_ab.push_back(db);
      inline_vb_ptr = c->scratch_vb.data();
      inline_vb_bytes = (u32)c->scratch_vb.size();
    }
  }

  // Handle indexing & primitive topology conversion
  trt_gl_draw_cmd cmd{};
  cmd.gl_prog = prog_id;
  cmd.indexed = indexed;
  cmd.instance_count = instance_count;
  cmd.base_vertex = base_vertex;
  cmd.inline_vb = inline_vb_ptr;
  cmd.inline_vb_bytes = inline_vb_bytes;
  cmd.attribs = active_ab.empty() ? nullptr : active_ab.data();
  cmd.attrib_count = (u32)active_ab.size();
  cmd.active_fbo = c->cur_draw_fbo;
  if (auto fit = c->fbos.find(c->cur_draw_fbo); fit != c->fbos.end()) {
    cmd.draw_buf_count = fit->second.draw_buf_count;
    memcpy(cmd.draw_bufs, fit->second.draw_bufs, sizeof(cmd.draw_bufs));
  }
  cmd.vs_cb0 = prog->vs_cb0.data();
  cmd.vs_cb0_bytes = (u32)prog->vs_cb0.size();
  cmd.ps_cb0 = prog->ps_cb0.data();
  cmd.ps_cb0_bytes = (u32)prog->ps_cb0.size();
  fill_sampler_and_ubo_state(c, *prog, cmd);
  cmd.rs = c->rs;

  if (!indexed) {
    u32 dprim = TRT_GL_PRIM_TRIANGLES;
    u32 dc = convert_nonindex_prim(mode, first, count, c->scratch_idx, dprim);
    cmd.prim_mode = dprim;
    if (!c->scratch_idx.empty()) {
      cmd.indexed = 1;
      cmd.use_inline_idx = 1;
      cmd.idx_type = GL_UNSIGNED_INT_V;
      cmd.inline_idx = c->scratch_idx.data();
      cmd.inline_idx_bytes = (u32)(c->scratch_idx.size() * 4u);
      cmd.count = (u32)c->scratch_idx.size();
      cmd.first = 0;
    } else {
      cmd.count = dc;
      cmd.first = first;
    }
  } else {
    const u32 ibo_id = vao->ibo;
    const u8* ib_bytes_ptr = nullptr;
    u64 ib_off = 0;
    if (ibo_id) {
      auto bit = c->share->bufs.find(ibo_id);
      if (bit != c->share->bufs.end()) {
        bit->second.kind_hint = TRT_GL_BUF_INDEX;
        sync_buf_to_host(ibo_id, bit->second);
        ib_off = (u64)(uintptr_t)idx_ptr_or_off;
        if (ib_off < bit->second.data.size()) ib_bytes_ptr = bit->second.data.data() + ib_off;
      }
    } else {
      ib_bytes_ptr = (const u8*)idx_ptr_or_off;
    }
    u32 dprim = mode;
    c->scratch_idx.clear();
    if (ib_bytes_ptr) {
      convert_indexed_prim(mode, ib_bytes_ptr, idx_type, count, c->scratch_idx, dprim);
    }
    cmd.prim_mode = dprim;
    if (!c->scratch_idx.empty()) {
      cmd.use_inline_idx = 1;
      cmd.idx_type = GL_UNSIGNED_INT_V;
      cmd.inline_idx = c->scratch_idx.data();
      cmd.inline_idx_bytes = (u32)(c->scratch_idx.size() * 4u);
      cmd.count = (u32)c->scratch_idx.size();
    } else if (ibo_id) {
      cmd.ibo_gl = ibo_id;
      cmd.ibo_offset = ib_off;
      cmd.idx_type = idx_type;
      cmd.count = count;
    } else {
      cmd.use_inline_idx = 1;
      cmd.inline_idx = idx_ptr_or_off;
      cmd.idx_type = idx_type;
      const u32 esz = (idx_type == GL_UNSIGNED_INT_V) ? 4u : 2u;
      cmd.inline_idx_bytes = count * esz;
      cmd.count = count;
    }
  }

  if (g_gl_hooks_set && g_gl_hooks.draw) g_gl_hooks.draw(&cmd, g_gl_hooks.user);
}

// Forward declare WGL/GL functions so wglGetProcAddress can return them
struct gl_proc_entry { const char* name; void* fn; };
extern const gl_proc_entry k_all_gl_procs[];
extern const size_t k_all_gl_proc_count;

// ---- WGL Implementations ---------------------------------------------------
void* TRT_GUEST_ABI trt_wglCreateContext(void* hdc) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = new wgl_ctx();
  c->hdc = hdc;
  c->share = new gl_share_group();
  init_ctx_defaults(c);
  g_contexts.push_back(c);
  opengl_log("opengl32: wglCreateContext -> created D3D11-backed GL context");
  return (void*)c;
}

void* TRT_GUEST_ABI trt_wglCreateLayerContext(void* hdc, int layer_plane) {
  (void)layer_plane;
  return trt_wglCreateContext(hdc);
}

void* TRT_GUEST_ABI trt_wglCreateContextAttribsARB(void* hdc, void* share_ctx,
                                                   const int* attrib_list) {
  (void)attrib_list;
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = new wgl_ctx();
  c->hdc = hdc;
  wgl_ctx* sh = (wgl_ctx*)share_ctx;
  if (sh && sh->alive && sh->share) {
    c->share = sh->share;
    c->share->refs++;
  } else {
    c->share = new gl_share_group();
  }
  init_ctx_defaults(c);
  g_contexts.push_back(c);
  opengl_log("opengl32: wglCreateContextAttribsARB -> created GL 4.3 compat D3D11 context");
  return (void*)c;
}

int TRT_GUEST_ABI trt_wglDeleteContext(void* hglrc) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = (wgl_ctx*)hglrc;
  if (!c) return 0;
  if (t_cur_ctx == c) t_cur_ctx = nullptr;
  auto it = std::find(g_contexts.begin(), g_contexts.end(), c);
  if (it != g_contexts.end()) g_contexts.erase(it);
  if (c->share) {
    if (--c->share->refs == 0) delete c->share;
  }
  delete c;
  return 1;
}

int TRT_GUEST_ABI trt_wglMakeCurrent(void* hdc, void* hglrc) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (!hglrc) {
    t_cur_ctx = nullptr;
    return 1;
  }
  wgl_ctx* c = (wgl_ctx*)hglrc;
  if (!c->alive) return 0;
  c->hdc = hdc;
  t_cur_ctx = c;
  return 1;
}

void* TRT_GUEST_ABI trt_wglGetCurrentContext(void) { return (void*)t_cur_ctx; }
void* TRT_GUEST_ABI trt_wglGetCurrentDC(void) {
  return t_cur_ctx ? t_cur_ctx->hdc : nullptr;
}

int TRT_GUEST_ABI trt_wglShareLists(void* ctx1, void* ctx2) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c1 = (wgl_ctx*)ctx1;
  wgl_ctx* c2 = (wgl_ctx*)ctx2;
  if (!c1 || !c2 || !c1->share) return 0;
  if (c2->share == c1->share) return 1;
  if (c2->share && --c2->share->refs == 0) delete c2->share;
  c2->share = c1->share;
  c1->share->refs++;
  return 1;
}

int TRT_GUEST_ABI trt_wglCopyContext(void*, void*, u32) { return 1; }
int TRT_GUEST_ABI trt_wglUseFontBitmapsA(void*, u32, u32, u32) { return 1; }
int TRT_GUEST_ABI trt_wglUseFontBitmapsW(void*, u32, u32, u32) { return 1; }
int TRT_GUEST_ABI trt_wglSwapLayerBuffers(void* hdc, u32) {
  return trt_wgl_swap_buffers(hdc);
}

int TRT_GUEST_ABI trt_wglChoosePixelFormat(void*, const void*) { return 1; }
int TRT_GUEST_ABI trt_wglSetPixelFormat(void* hdc, int fmt, const void*) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  g_pixel_formats[hdc] = fmt ? fmt : 1;
  return 1;
}
int TRT_GUEST_ABI trt_wglGetPixelFormat(void* hdc) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  auto it = g_pixel_formats.find(hdc);
  return (it != g_pixel_formats.end()) ? it->second : 1;
}
int TRT_GUEST_ABI trt_wglDescribePixelFormat(void*, int, u32 bytes, void* ppfd) {
  if (ppfd && bytes >= 40) {
    memset(ppfd, 0, bytes);
    u8* p = (u8*)ppfd;
    *(u16*)(p + 0) = 40;
    *(u16*)(p + 2) = 1;
    *(u32*)(p + 4) = 0x25; // DRAW_TO_WINDOW | SUPPORT_OPENGL | DOUBLEBUFFER
    p[9] = 32; p[10] = 8; p[12] = 8; p[14] = 8; p[16] = 8;
    p[23] = 24; p[24] = 8;
  }
  return 1;
}

const char* TRT_GUEST_ABI trt_wglGetExtensionsStringARB(void*) {
  return "WGL_ARB_extensions_string WGL_EXT_extensions_string "
         "WGL_ARB_pixel_format WGL_ARB_create_context "
         "WGL_ARB_create_context_profile WGL_EXT_swap_control "
         "WGL_EXT_swap_control_tear WGL_ARB_multisample WGL_ARB_framebuffer_sRGB";
}
const char* TRT_GUEST_ABI trt_wglGetExtensionsStringEXT(void) {
  return trt_wglGetExtensionsStringARB(nullptr);
}

int TRT_GUEST_ABI trt_wglChoosePixelFormatARB(void*, const int*, const float*,
                                              u32 max_fmts, int* formats,
                                              u32* num_formats) {
  if (num_formats) *num_formats = 1;
  if (formats && max_fmts > 0) formats[0] = 1;
  return 1;
}

int TRT_GUEST_ABI trt_wglGetPixelFormatAttribivARB(void*, int, int, u32 count,
                                                   const int* attrs, int* vals) {
  if (!attrs || !vals) return 0;
  for (u32 i = 0; i < count; ++i) {
    switch (attrs[i]) {
      case 0x2001: /*DRAW_TO_WINDOW*/ vals[i] = 1; break;
      case 0x2003: /*ACCELERATION*/ vals[i] = 0x2027; break; // FULL
      case 0x2010: /*SUPPORT_OPENGL*/ vals[i] = 1; break;
      case 0x2011: /*DOUBLE_BUFFER*/ vals[i] = 1; break;
      case 0x2013: /*PIXEL_TYPE*/ vals[i] = 0x202B; break; // RGBA
      case 0x2014: /*COLOR_BITS*/ vals[i] = 32; break;
      case 0x2015: case 0x2017: case 0x2019: case 0x201B: vals[i] = 8; break;
      case 0x2022: /*DEPTH_BITS*/ vals[i] = 24; break;
      case 0x2023: /*STENCIL_BITS*/ vals[i] = 8; break;
      default: vals[i] = 0; break;
    }
  }
  return 1;
}

int TRT_GUEST_ABI trt_wglSwapIntervalEXT(int interval) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) c->swap_interval = interval;
  return 1;
}
int TRT_GUEST_ABI trt_wglGetSwapIntervalEXT(void) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  return c ? c->swap_interval : 1;
}

void* TRT_GUEST_ABI trt_wglGetProcAddress(const char* name) {
  if (!name || !name[0]) return nullptr;
  for (size_t i = 0; i < k_all_gl_proc_count; ++i) {
    if (strcmp(k_all_gl_procs[i].name, name) == 0) return k_all_gl_procs[i].fn;
  }
  return nullptr;
}

// ---- GL Core State & Queries -----------------------------------------------
u32 TRT_GUEST_ABI trt_glGetError(void) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return GL_NO_ERROR_V;
  u32 e = c->last_error;
  c->last_error = GL_NO_ERROR_V;
  return e;
}

const u8* TRT_GUEST_ABI trt_glGetString(u32 name) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  switch (name) {
    case GL_VENDOR_V: return (const u8*)"Tiramisu (D3D11 Translation)";
    case GL_RENDERER_V: return (const u8*)"Tiramisu GL-on-D3D11";
    case GL_VERSION_V: return (const u8*)"4.3.0 Tiramisu GLSL->HLSL";
    case GL_SHADING_LANGUAGE_VERSION_V: return (const u8*)"4.30 Tiramisu";
    case GL_EXTENSIONS_V:
      if (g_ext_string_cache.empty()) g_ext_string_cache = build_ext_string();
      return (const u8*)g_ext_string_cache.c_str();
    default: return (const u8*)"";
  }
}

const u8* TRT_GUEST_ABI trt_glGetStringi(u32 name, u32 index) {
  if (name == GL_EXTENSIONS_V) {
    const u32 n = (u32)(sizeof(k_gl_exts) / sizeof(k_gl_exts[0]));
    if (index < n) return (const u8*)k_gl_exts[index];
  }
  return nullptr;
}

void TRT_GUEST_ABI trt_glGetIntegerv(u32 pname, int* data) {
  if (!data) return;
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  switch (pname) {
    case GL_MAJOR_VERSION_V: data[0] = 4; break;
    case GL_MINOR_VERSION_V: data[0] = 3; break;
    case GL_NUM_EXTENSIONS_V: data[0] = (int)(sizeof(k_gl_exts) / sizeof(k_gl_exts[0])); break;
    case GL_MAX_TEXTURE_SIZE_V: data[0] = 16384; break;
    case GL_MAX_TEXTURE_IMAGE_UNITS_V: data[0] = 16; break;
    case GL_MAX_VERTEX_ATTRIBS_V: data[0] = 16; break;
    case GL_MAX_COLOR_ATTACHMENTS_V:
    case GL_MAX_DRAW_BUFFERS_V: data[0] = 8; break;
    case GL_VIEWPORT_V:
      if (c) { memcpy(data, c->rs.vp, sizeof(int) * 4); }
      break;
    case GL_SCISSOR_BOX_V:
      if (c) { memcpy(data, c->rs.sc, sizeof(int) * 4); }
      break;
    case GL_CURRENT_PROGRAM_V: data[0] = c ? (int)c->cur_prog : 0; break;
    case GL_VERTEX_ARRAY_BINDING_V: data[0] = c ? (int)c->cur_vao : 0; break;
    case GL_ARRAY_BUFFER_BINDING_V: data[0] = c ? (int)c->cur_vbo : 0; break;
    case GL_ELEMENT_ARRAY_BUFFER_BINDING_V:
      data[0] = c ? (int)get_cur_vao(c)->ibo : 0; break;
    case GL_FRAMEBUFFER_BINDING_V: data[0] = c ? (int)c->cur_draw_fbo : 0; break;
    case GL_ACTIVE_TEXTURE_V: data[0] = c ? (int)(0x84C0u + c->active_tex_unit) : 0x84C0; break;
    case GL_TEXTURE_BINDING_2D_V:
      data[0] = c ? (int)c->bound_tex_2d[std::min(c->active_tex_unit, 31u)] : 0; break;
    case 0x8037: data[0] = 4; break; // GL_SAMPLE_BUFFERS / context flags fallback
    case 0x821E: data[0] = 2; break; // GL_CONTEXT_FLAGS
    case 0x9126: data[0] = 2; break; // GL_CONTEXT_PROFILE_MASK = COMPATIBILITY
    default: data[0] = 0; break;
  }
}

void TRT_GUEST_ABI trt_glGetInteger64v(u32 pname, i64* data) {
  int v = 0;
  trt_glGetIntegerv(pname, &v);
  if (data) *data = v;
}
void TRT_GUEST_ABI trt_glGetIntegeri_v(u32 pname, u32 idx, int* data) {
  if (!data) return;
  if (pname == 0x91BE) { // GL_MAX_COMPUTE_WORK_GROUP_COUNT
    data[0] = (idx == 2) ? 65535 : 65535;
    return;
  }
  if (pname == 0x91BF) { data[0] = (idx == 2) ? 64 : 1024; return; }
  trt_glGetIntegerv(pname, data);
}
void TRT_GUEST_ABI trt_glGetBooleanv(u32 pname, u8* data) {
  int v[4] = {};
  trt_glGetIntegerv(pname, v);
  if (data) *data = v[0] ? 1 : 0;
}
void TRT_GUEST_ABI trt_glGetFloatv(u32 pname, float* data) {
  if (!data) return;
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) { *data = 0.0f; return; }
  if (pname == 0x0BA6) { memcpy(data, c->mv_stack.back().m, 64); return; }
  if (pname == 0x0BA7) { memcpy(data, c->proj_stack.back().m, 64); return; }
  if (pname == GL_VIEWPORT_V) {
    for (int i = 0; i < 4; ++i) data[i] = (float)c->rs.vp[i];
    return;
  }
  int iv = 0;
  trt_glGetIntegerv(pname, &iv);
  *data = (float)iv;
}
void TRT_GUEST_ABI trt_glGetDoublev(u32 pname, double* data) {
  float f[16] = {};
  trt_glGetFloatv(pname, f);
  if (data) *data = (double)f[0];
}

static void set_cap(wgl_ctx* c, u32 cap, bool en) {
  switch (cap) {
    case GL_BLEND_V: c->rs.blend_en = en ? 1 : 0; break;
    case GL_DEPTH_TEST_V: c->rs.depth_test = en ? 1 : 0; break;
    case GL_CULL_FACE_V: c->rs.cull_en = en ? 1 : 0; break;
    case GL_SCISSOR_TEST_V: c->rs.scissor_en = en ? 1 : 0; break;
    case GL_STENCIL_TEST_V: c->rs.stencil_en = en ? 1 : 0; break;
    case GL_TEXTURE_2D_V:
      if (c->active_tex_unit < 8u) c->tex2d_enabled[c->active_tex_unit] = en;
      break;
    case GL_LIGHTING_V: c->lighting_en = en; break;
    case GL_ALPHA_TEST_V: c->alpha_test_en = en; break;
    case GL_FOG_V: c->fog_en = en; break;
    case GL_FRAMEBUFFER_SRGB_V: c->rs.srgb_en = en ? 1 : 0; break;
    default: break;
  }
}

void TRT_GUEST_ABI trt_glEnable(u32 cap) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  if (c->recording_list) {
    c->share->dls[c->recording_list].ops.push_back({DL_OP_ENABLE, cap, 0, {}, {}});
    if (!c->recording_exec) return;
  }
  set_cap(c, cap, true);
}
void TRT_GUEST_ABI trt_glDisable(u32 cap) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  if (c->recording_list) {
    c->share->dls[c->recording_list].ops.push_back({DL_OP_DISABLE, cap, 0, {}, {}});
    if (!c->recording_exec) return;
  }
  set_cap(c, cap, false);
}
u8 TRT_GUEST_ABI trt_glIsEnabled(u32 cap) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return 0;
  switch (cap) {
    case GL_BLEND_V: return c->rs.blend_en;
    case GL_DEPTH_TEST_V: return c->rs.depth_test;
    case GL_CULL_FACE_V: return c->rs.cull_en;
    case GL_SCISSOR_TEST_V: return c->rs.scissor_en;
    case GL_STENCIL_TEST_V: return c->rs.stencil_en;
    case GL_TEXTURE_2D_V: return c->active_tex_unit < 8u ? (c->tex2d_enabled[c->active_tex_unit] ? 1 : 0) : 0;
    case GL_LIGHTING_V: return c->lighting_en ? 1 : 0;
    case GL_ALPHA_TEST_V: return c->alpha_test_en ? 1 : 0;
    case GL_FOG_V: return c->fog_en ? 1 : 0;
    default: return 0;
  }
}

void TRT_GUEST_ABI trt_glViewport(int x, int y, int w, int h) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) {
    c->rs.vp[0] = x; c->rs.vp[1] = y; c->rs.vp[2] = w; c->rs.vp[3] = h;
  }
}
void TRT_GUEST_ABI trt_glScissor(int x, int y, int w, int h) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) {
    c->rs.sc[0] = x; c->rs.sc[1] = y; c->rs.sc[2] = w; c->rs.sc[3] = h;
  }
}
void TRT_GUEST_ABI trt_glDepthRange(double zmin, double zmax) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) {
    c->rs.depth_min = (float)zmin;
    c->rs.depth_max = (float)zmax;
  }
}
void TRT_GUEST_ABI trt_glDepthRangef(float zmin, float zmax) {
  trt_glDepthRange(zmin, zmax);
}
void TRT_GUEST_ABI trt_glClearColor(float r, float g, float b, float a) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) {
    c->clear_color[0] = r; c->clear_color[1] = g;
    c->clear_color[2] = b; c->clear_color[3] = a;
  }
}
void TRT_GUEST_ABI trt_glClearDepth(double d) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) c->clear_depth = (float)d;
}
void TRT_GUEST_ABI trt_glClearDepthf(float d) { trt_glClearDepth(d); }
void TRT_GUEST_ABI trt_glClearStencil(int s) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) c->clear_stencil = s;
}
void TRT_GUEST_ABI trt_glClear(u32 mask) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  sync_active_fbo(c);
  u32 trt_m = 0;
  if (mask & GL_COLOR_BUFFER_BIT_V) trt_m |= 1u;
  if (mask & GL_DEPTH_BUFFER_BIT_V) trt_m |= 2u;
  if (mask & GL_STENCIL_BUFFER_BIT_V) trt_m |= 4u;
  if (trt_m && g_gl_hooks_set && g_gl_hooks.clear) {
    g_gl_hooks.clear(trt_m, c->clear_color, c->clear_depth, c->clear_stencil,
                     g_gl_hooks.user);
  }
}

void TRT_GUEST_ABI trt_glColorMask(u8 r, u8 g, u8 b, u8 a) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) {
    c->rs.color_mask = (r ? 1u : 0u) | (g ? 2u : 0u) | (b ? 4u : 0u) | (a ? 8u : 0u);
  }
}
void TRT_GUEST_ABI trt_glDepthMask(u8 flag) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) c->rs.depth_write = flag ? 1 : 0;
}
void TRT_GUEST_ABI trt_glDepthFunc(u32 func) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) c->rs.depth_func = func;
}
void TRT_GUEST_ABI trt_glCullFace(u32 mode) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) c->rs.cull_face = mode;
}
void TRT_GUEST_ABI trt_glFrontFace(u32 mode) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) c->rs.front_ccw = (mode == GL_CCW_V) ? 1 : 0;
}
void TRT_GUEST_ABI trt_glPolygonMode(u32 face, u32 mode) {
  (void)face;
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) c->rs.wireframe = (mode == GL_LINE_V) ? 1 : 0;
}
void TRT_GUEST_ABI trt_glBlendFunc(u32 s, u32 d) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) {
    c->rs.blend_src_rgb = c->rs.blend_src_a = s;
    c->rs.blend_dst_rgb = c->rs.blend_dst_a = d;
  }
}
void TRT_GUEST_ABI trt_glBlendFuncSeparate(u32 sr, u32 dr, u32 sa, u32 da) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) {
    c->rs.blend_src_rgb = sr; c->rs.blend_dst_rgb = dr;
    c->rs.blend_src_a = sa; c->rs.blend_dst_a = da;
  }
}
void TRT_GUEST_ABI trt_glBlendEquation(u32 mode) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) c->rs.blend_eq_rgb = c->rs.blend_eq_a = mode;
}
void TRT_GUEST_ABI trt_glBlendEquationSeparate(u32 mr, u32 ma) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) { c->rs.blend_eq_rgb = mr; c->rs.blend_eq_a = ma; }
}
void TRT_GUEST_ABI trt_glBlendColor(float r, float g, float b, float a) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) {
    c->rs.blend_const[0] = r; c->rs.blend_const[1] = g;
    c->rs.blend_const[2] = b; c->rs.blend_const[3] = a;
  }
}
void TRT_GUEST_ABI trt_glAlphaFunc(u32 func, float ref) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) { c->alpha_func = func; c->alpha_ref = ref; }
}
void TRT_GUEST_ABI trt_glStencilFunc(u32 func, int ref, u32 mask) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) {
    c->rs.stencil_func = func; c->rs.stencil_ref = ref; c->rs.stencil_mask = mask;
  }
}
void TRT_GUEST_ABI trt_glStencilMask(u32 mask) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) c->rs.stencil_wmask = mask;
}
void TRT_GUEST_ABI trt_glStencilOp(u32 fail, u32 zfail, u32 zpass) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) {
    c->rs.stencil_sfail = fail; c->rs.stencil_dpfail = zfail; c->rs.stencil_dppass = zpass;
  }
}
void TRT_GUEST_ABI trt_glStencilFuncSeparate(u32, u32 func, int ref, u32 mask) {
  trt_glStencilFunc(func, ref, mask);
}
void TRT_GUEST_ABI trt_glStencilOpSeparate(u32, u32 sf, u32 df, u32 dp) {
  trt_glStencilOp(sf, df, dp);
}
void TRT_GUEST_ABI trt_glStencilMaskSeparate(u32, u32 m) { trt_glStencilMask(m); }

void TRT_GUEST_ABI trt_glPixelStorei(u32 pname, int param) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  if (pname == GL_UNPACK_ROW_LENGTH_V) c->unpack_row_len = param;
  else if (pname == GL_UNPACK_SKIP_ROWS_V) c->unpack_skip_rows = param;
  else if (pname == GL_UNPACK_SKIP_PIXELS_V) c->unpack_skip_pixels = param;
  else if (pname == GL_UNPACK_ALIGNMENT_V) c->unpack_align = param;
}
void TRT_GUEST_ABI trt_glPixelStoref(u32 pname, float p) {
  trt_glPixelStorei(pname, (int)p);
}

void TRT_GUEST_ABI trt_glFogf(u32 pname, float p) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  if (pname == GL_FOG_MODE_V) c->fog_mode = (u32)p;
  else if (pname == GL_FOG_DENSITY_V) c->fog_density = p;
  else if (pname == GL_FOG_START_V) c->fog_start = p;
  else if (pname == GL_FOG_END_V) c->fog_end = p;
}
void TRT_GUEST_ABI trt_glFogi(u32 pname, int p) { trt_glFogf(pname, (float)p); }
void TRT_GUEST_ABI trt_glFogfv(u32 pname, const float* params) {
  if (!params) return;
  if (pname == GL_FOG_COLOR_V) {
    std::lock_guard<std::mutex> lk(g_gl_mu);
    if (wgl_ctx* c = cur_ctx()) memcpy(c->fog_color, params, 16);
  } else {
    trt_glFogf(pname, params[0]);
  }
}
void TRT_GUEST_ABI trt_glLightfv(u32 light, u32 pname, const float* params) {
  if (!params || light != GL_LIGHT0_V) return;
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  if (pname == GL_DIFFUSE_V) memcpy(c->light0_diffuse, params, 16);
  else if (pname == GL_AMBIENT_V) memcpy(c->light0_amb, params, 16);
  else if (pname == GL_POSITION_V) memcpy(c->light0_pos, params, 16);
}
void TRT_GUEST_ABI trt_glLightModelfv(u32 pname, const float* params) {
  if (!params || pname != GL_LIGHT_MODEL_AMBIENT_V) return;
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) memcpy(c->light_ambient, params, 16);
}
void TRT_GUEST_ABI trt_glMaterialfv(u32, u32 pname, const float* params) {
  if (!params) return;
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  if (pname == GL_DIFFUSE_V) memcpy(c->mat_diffuse, params, 16);
  else if (pname == GL_AMBIENT_V) memcpy(c->mat_ambient, params, 16);
}

void TRT_GUEST_ABI trt_glReadPixels(int x, int y, int w, int h, u32 fmt,
                                    u32 type, void* pixels) {
  (void)fmt; (void)type;
  if (!pixels || w <= 0 || h <= 0) return;
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (c && g_gl_hooks_set && g_gl_hooks.read_pixels) {
    g_gl_hooks.read_pixels(c->cur_read_fbo, x, y, w, h, pixels, g_gl_hooks.user);
  } else {
    memset(pixels, 0, (size_t)w * (size_t)h * 4u);
  }
}

void TRT_GUEST_ABI trt_glHint(u32, u32) {}
void TRT_GUEST_ABI trt_glFinish(void) {}
void TRT_GUEST_ABI trt_glFlush(void) {}
void TRT_GUEST_ABI trt_glPolygonOffset(float, float) {}
void TRT_GUEST_ABI trt_glLineWidth(float) {}
void TRT_GUEST_ABI trt_glPointSize(float) {}
void TRT_GUEST_ABI trt_glShadeModel(u32) {}
void TRT_GUEST_ABI trt_glReadBuffer(u32 mode) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) c->fbos[c->cur_read_fbo].read_buf = mode;
}
void TRT_GUEST_ABI trt_glDrawBuffer(u32 mode) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) {
    auto& f = c->fbos[c->cur_draw_fbo];
    f.draw_buf_count = 1;
    f.draw_bufs[0] = mode;
  }
}
void TRT_GUEST_ABI trt_glDrawBuffers(int n, const u32* bufs) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) {
    auto& f = c->fbos[c->cur_draw_fbo];
    f.draw_buf_count = (u32)std::max(0, std::min(8, n));
    for (u32 i = 0; i < f.draw_buf_count; ++i) f.draw_bufs[i] = bufs[i];
  }
}
void TRT_GUEST_ABI trt_glPushAttrib(u32) {}
void TRT_GUEST_ABI trt_glPopAttrib(void) {}
void TRT_GUEST_ABI trt_glPushClientAttrib(u32) {}
void TRT_GUEST_ABI trt_glPopClientAttrib(void) {}

// ---- Matrix Stack ----------------------------------------------------------
void TRT_GUEST_ABI trt_glMatrixMode(u32 mode) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) c->matrix_mode = mode;
}
void TRT_GUEST_ABI trt_glLoadIdentity(void) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  if (c->recording_list) {
    c->share->dls[c->recording_list].ops.push_back({DL_OP_LOAD_IDENTITY, c->matrix_mode, 0, {}, {}});
    if (!c->recording_exec) return;
  }
  active_mat_stack(c).back() = mat4_identity();
}
void TRT_GUEST_ABI trt_glLoadMatrixf(const float* m) {
  if (!m) return;
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  if (c->recording_list) {
    gl_dl_op op{DL_OP_LOAD_MATRIX, c->matrix_mode, 0, {}, {}};
    memcpy(op.f, m, 64);
    c->share->dls[c->recording_list].ops.push_back(std::move(op));
    if (!c->recording_exec) return;
  }
  memcpy(active_mat_stack(c).back().m, m, 64);
}
void TRT_GUEST_ABI trt_glLoadMatrixd(const double* m) {
  if (!m) return;
  float mf[16];
  for (int i = 0; i < 16; ++i) mf[i] = (float)m[i];
  trt_glLoadMatrixf(mf);
}
void TRT_GUEST_ABI trt_glMultMatrixf(const float* m) {
  if (!m) return;
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  if (c->recording_list) {
    gl_dl_op op{DL_OP_MULT_MATRIX, c->matrix_mode, 0, {}, {}};
    memcpy(op.f, m, 64);
    c->share->dls[c->recording_list].ops.push_back(std::move(op));
    if (!c->recording_exec) return;
  }
  gl_mat4 rhs;
  memcpy(rhs.m, m, 64);
  auto& st = active_mat_stack(c);
  st.back() = mat4_mul(st.back(), rhs);
}
void TRT_GUEST_ABI trt_glMultMatrixd(const double* m) {
  if (!m) return;
  float mf[16];
  for (int i = 0; i < 16; ++i) mf[i] = (float)m[i];
  trt_glMultMatrixf(mf);
}
void TRT_GUEST_ABI trt_glPushMatrix(void) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  if (c->recording_list) {
    c->share->dls[c->recording_list].ops.push_back({DL_OP_PUSH_MATRIX, c->matrix_mode, 0, {}, {}});
    if (!c->recording_exec) return;
  }
  auto& st = active_mat_stack(c);
  if (st.size() >= 64) { set_gl_err(c, GL_STACK_OVERFLOW_V); return; }
  st.push_back(st.back());
}
void TRT_GUEST_ABI trt_glPopMatrix(void) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  if (c->recording_list) {
    c->share->dls[c->recording_list].ops.push_back({DL_OP_POP_MATRIX, c->matrix_mode, 0, {}, {}});
    if (!c->recording_exec) return;
  }
  auto& st = active_mat_stack(c);
  if (st.size() <= 1) { set_gl_err(c, GL_STACK_UNDERFLOW_V); return; }
  st.pop_back();
}
void TRT_GUEST_ABI trt_glOrtho(double l, double r, double b, double t, double n, double f) {
  if (l == r || b == t || n == f) return;
  gl_mat4 m{};
  m.m[0] = (float)(2.0 / (r - l));
  m.m[5] = (float)(2.0 / (t - b));
  m.m[10] = (float)(-2.0 / (f - n));
  m.m[12] = (float)(-(r + l) / (r - l));
  m.m[13] = (float)(-(t + b) / (t - b));
  m.m[14] = (float)(-(f + n) / (f - n));
  m.m[15] = 1.0f;
  trt_glMultMatrixf(m.m);
}
void TRT_GUEST_ABI trt_glFrustum(double l, double r, double b, double t, double n, double f) {
  if (l == r || b == t || n == f || n <= 0 || f <= 0) return;
  gl_mat4 m{};
  m.m[0] = (float)((2.0 * n) / (r - l));
  m.m[5] = (float)((2.0 * n) / (t - b));
  m.m[8] = (float)((r + l) / (r - l));
  m.m[9] = (float)((t + b) / (t - b));
  m.m[10] = (float)(-(f + n) / (f - n));
  m.m[11] = -1.0f;
  m.m[14] = (float)(-(2.0 * f * n) / (f - n));
  trt_glMultMatrixf(m.m);
}
void TRT_GUEST_ABI trt_glTranslatef(float x, float y, float z) {
  gl_mat4 m = mat4_identity();
  m.m[12] = x; m.m[13] = y; m.m[14] = z;
  trt_glMultMatrixf(m.m);
}
void TRT_GUEST_ABI trt_glTranslated(double x, double y, double z) {
  trt_glTranslatef((float)x, (float)y, (float)z);
}
void TRT_GUEST_ABI trt_glScalef(float x, float y, float z) {
  gl_mat4 m{};
  m.m[0] = x; m.m[5] = y; m.m[10] = z; m.m[15] = 1.0f;
  trt_glMultMatrixf(m.m);
}
void TRT_GUEST_ABI trt_glScaled(double x, double y, double z) {
  trt_glScalef((float)x, (float)y, (float)z);
}
void TRT_GUEST_ABI trt_glRotatef(float angle, float x, float y, float z) {
  float len = sqrtf(x * x + y * y + z * z);
  if (len < 1e-8f) return;
  x /= len; y /= len; z /= len;
  const float rad = angle * 3.14159265358979323846f / 180.0f;
  const float c = cosf(rad), s = sinf(rad), ic = 1.0f - c;
  gl_mat4 m{};
  m.m[0] = x * x * ic + c;     m.m[4] = x * y * ic - z * s; m.m[8] = x * z * ic + y * s;
  m.m[1] = y * x * ic + z * s; m.m[5] = y * y * ic + c;     m.m[9] = y * z * ic - x * s;
  m.m[2] = z * x * ic - y * s; m.m[6] = z * y * ic + x * s; m.m[10] = z * z * ic + c;
  m.m[15] = 1.0f;
  trt_glMultMatrixf(m.m);
}
void TRT_GUEST_ABI trt_glRotated(double a, double x, double y, double z) {
  trt_glRotatef((float)a, (float)x, (float)y, (float)z);
}

// ---- Immediate Mode & Display Lists ----------------------------------------
void TRT_GUEST_ABI trt_glBegin(u32 mode) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  c->in_begin = true;
  c->immed_mode = mode;
  c->immed_verts.clear();
}
void TRT_GUEST_ABI trt_glEnd(void) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c || !c->in_begin) return;
  c->in_begin = false;
  if (c->recording_list) {
    gl_dl_op op;
    op.kind = DL_OP_DRAW_IMMED;
    op.u = c->immed_mode;
    op.verts = c->immed_verts;
    c->share->dls[c->recording_list].ops.push_back(std::move(op));
    if (!c->recording_exec) {
      c->immed_verts.clear();
      return;
    }
  }
  emit_immediate_verts(c, c->immed_mode, c->immed_verts);
  c->immed_verts.clear();
}
static void push_immed_v(float x, float y, float z, float w) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c || !c->in_begin) return;
  gl_ff_vert v{};
  v.pos[0] = x; v.pos[1] = y; v.pos[2] = z; v.pos[3] = w;
  memcpy(v.nrm, c->cur_attrib[2], 12);
  memcpy(v.col, c->cur_attrib[3], 16);
  memcpy(v.tc0, c->cur_attrib[8], 16);
  memcpy(v.tc1, c->cur_attrib[9], 16);
  c->immed_verts.push_back(v);
}
void TRT_GUEST_ABI trt_glVertex2f(float x, float y) { push_immed_v(x, y, 0.0f, 1.0f); }
void TRT_GUEST_ABI trt_glVertex2i(int x, int y) { push_immed_v((float)x, (float)y, 0.0f, 1.0f); }
void TRT_GUEST_ABI trt_glVertex2d(double x, double y) { push_immed_v((float)x, (float)y, 0.0f, 1.0f); }
void TRT_GUEST_ABI trt_glVertex2fv(const float* v) { if (v) push_immed_v(v[0], v[1], 0.0f, 1.0f); }
void TRT_GUEST_ABI trt_glVertex3f(float x, float y, float z) { push_immed_v(x, y, z, 1.0f); }
void TRT_GUEST_ABI trt_glVertex3d(double x, double y, double z) { push_immed_v((float)x, (float)y, (float)z, 1.0f); }
void TRT_GUEST_ABI trt_glVertex3fv(const float* v) { if (v) push_immed_v(v[0], v[1], v[2], 1.0f); }
void TRT_GUEST_ABI trt_glVertex4f(float x, float y, float z, float w) { push_immed_v(x, y, z, w); }
void TRT_GUEST_ABI trt_glVertex4fv(const float* v) { if (v) push_immed_v(v[0], v[1], v[2], v[3]); }
void TRT_GUEST_ABI trt_glNormal3f(float x, float y, float z) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) { c->cur_attrib[2][0] = x; c->cur_attrib[2][1] = y; c->cur_attrib[2][2] = z; }
}
void TRT_GUEST_ABI trt_glNormal3fv(const float* v) { if (v) trt_glNormal3f(v[0], v[1], v[2]); }
void TRT_GUEST_ABI trt_glColor4f(float r, float g, float b, float a) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) {
    c->cur_attrib[3][0] = r; c->cur_attrib[3][1] = g;
    c->cur_attrib[3][2] = b; c->cur_attrib[3][3] = a;
  }
}
void TRT_GUEST_ABI trt_glColor3f(float r, float g, float b) { trt_glColor4f(r, g, b, 1.0f); }
void TRT_GUEST_ABI trt_glColor3fv(const float* v) { if (v) trt_glColor4f(v[0], v[1], v[2], 1.0f); }
void TRT_GUEST_ABI trt_glColor4fv(const float* v) { if (v) trt_glColor4f(v[0], v[1], v[2], v[3]); }
void TRT_GUEST_ABI trt_glColor3ub(u8 r, u8 g, u8 b) {
  trt_glColor4f(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
}
void TRT_GUEST_ABI trt_glColor4ub(u8 r, u8 g, u8 b, u8 a) {
  trt_glColor4f(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}
void TRT_GUEST_ABI trt_glColor4ubv(const u8* v) { if (v) trt_glColor4ub(v[0], v[1], v[2], v[3]); }
void TRT_GUEST_ABI trt_glTexCoord2f(float s, float t) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) {
    c->cur_attrib[8][0] = s; c->cur_attrib[8][1] = t; c->cur_attrib[8][2] = 0; c->cur_attrib[8][3] = 1;
  }
}
void TRT_GUEST_ABI trt_glTexCoord2d(double s, double t) { trt_glTexCoord2f((float)s, (float)t); }
void TRT_GUEST_ABI trt_glTexCoord2fv(const float* v) { if (v) trt_glTexCoord2f(v[0], v[1]); }
void TRT_GUEST_ABI trt_glMultiTexCoord2f(u32 unit_gl, float s, float t) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) {
    u32 u = (unit_gl >= 0x84C0u) ? std::min(unit_gl - 0x84C0u, 7u) : 0u;
    c->cur_attrib[8u + u][0] = s; c->cur_attrib[8u + u][1] = t;
    c->cur_attrib[8u + u][2] = 0; c->cur_attrib[8u + u][3] = 1;
  }
}

u32 TRT_GUEST_ABI trt_glGenLists(int range) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c || range <= 0) return 0;
  u32 base = c->share->next_dl;
  c->share->next_dl += (u32)range;
  for (int i = 0; i < range; ++i) c->share->dls[base + (u32)i].alive = true;
  return base;
}
void TRT_GUEST_ABI trt_glDeleteLists(u32 list, int range) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  for (int i = 0; i < range; ++i) c->share->dls.erase(list + (u32)i);
}
void TRT_GUEST_ABI trt_glNewList(u32 list, u32 mode) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  c->share->dls[list].alive = true;
  c->share->dls[list].ops.clear();
  c->recording_list = list;
  c->recording_exec = (mode == 0x1301u); // GL_COMPILE_AND_EXECUTE
}
void TRT_GUEST_ABI trt_glEndList(void) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) { c->recording_list = 0; c->recording_exec = false; }
}
void TRT_GUEST_ABI trt_glCallList(u32 list) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  auto it = c->share->dls.find(list);
  if (it == c->share->dls.end() || !it->second.alive) return;
  const std::vector<gl_dl_op> ops = it->second.ops;
  u32 prev_mode = c->matrix_mode;
  for (const auto& op : ops) {
    switch (op.kind) {
      case DL_OP_DRAW_IMMED: emit_immediate_verts(c, op.u, op.verts); break;
      case DL_OP_LOAD_IDENTITY:
        c->matrix_mode = op.u; active_mat_stack(c).back() = mat4_identity(); break;
      case DL_OP_LOAD_MATRIX:
        c->matrix_mode = op.u; memcpy(active_mat_stack(c).back().m, op.f, 64); break;
      case DL_OP_MULT_MATRIX: {
        c->matrix_mode = op.u; gl_mat4 rhs; memcpy(rhs.m, op.f, 64);
        auto& st = active_mat_stack(c); st.back() = mat4_mul(st.back(), rhs);
      } break;
      case DL_OP_PUSH_MATRIX: {
        c->matrix_mode = op.u; auto& st = active_mat_stack(c);
        if (st.size() < 64) st.push_back(st.back());
      } break;
      case DL_OP_POP_MATRIX: {
        c->matrix_mode = op.u; auto& st = active_mat_stack(c);
        if (st.size() > 1) st.pop_back();
      } break;
      case DL_OP_BIND_TEX:
        if (op.u == GL_TEXTURE_2D_V) c->bound_tex_2d[std::min(c->active_tex_unit, 31u)] = op.u2;
        break;
      case DL_OP_ENABLE: set_cap(c, op.u, true); break;
      case DL_OP_DISABLE: set_cap(c, op.u, false); break;
      default: break;
    }
  }
  c->matrix_mode = prev_mode;
}
void TRT_GUEST_ABI trt_glListBase(u32 base) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) c->list_base = base;
}
void TRT_GUEST_ABI trt_glCallLists(int n, u32 type, const void* lists) {
  if (!lists || n <= 0) return;
  wgl_ctx* c = cur_ctx();
  u32 base = c ? c->list_base : 0;
  for (int i = 0; i < n; ++i) {
    u32 id = base;
    if (type == GL_UNSIGNED_BYTE_V) id += ((const u8*)lists)[i];
    else if (type == GL_UNSIGNED_SHORT_V) id += ((const u16*)lists)[i];
    else if (type == GL_UNSIGNED_INT_V) id += ((const u32*)lists)[i];
    else id += ((const u8*)lists)[i];
    trt_glCallList(id);
  }
}
u8 TRT_GUEST_ABI trt_glIsList(u32 list) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  return (c && c->share->dls.count(list)) ? 1 : 0;
}

// ---- Client arrays & draws -------------------------------------------------
void TRT_GUEST_ABI trt_glClientActiveTexture(u32 tex) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) {
    c->client_active_tex = (tex >= 0x84C0u) ? std::min(tex - 0x84C0u, 7u) : 0u;
  }
}
void TRT_GUEST_ABI trt_glEnableClientState(u32 arr) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  if (arr == GL_VERTEX_ARRAY_V) c->ff_pos.enabled = true;
  else if (arr == GL_NORMAL_ARRAY_V) c->ff_nrm.enabled = true;
  else if (arr == GL_COLOR_ARRAY_V) c->ff_col.enabled = true;
  else if (arr == GL_TEXTURE_COORD_ARRAY_V) c->ff_tc[std::min(c->client_active_tex, 7u)].enabled = true;
}
void TRT_GUEST_ABI trt_glDisableClientState(u32 arr) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  if (arr == GL_VERTEX_ARRAY_V) c->ff_pos.enabled = false;
  else if (arr == GL_NORMAL_ARRAY_V) c->ff_nrm.enabled = false;
  else if (arr == GL_COLOR_ARRAY_V) c->ff_col.enabled = false;
  else if (arr == GL_TEXTURE_COORD_ARRAY_V) c->ff_tc[std::min(c->client_active_tex, 7u)].enabled = false;
}
void TRT_GUEST_ABI trt_glVertexPointer(int size, u32 type, int stride, const void* ptr) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) {
    c->ff_pos.comps = (u32)size; c->ff_pos.type = type;
    c->ff_pos.stride = (u32)stride; c->ff_pos.ptr = ptr; c->ff_pos.vbo = c->cur_vbo;
  }
}
void TRT_GUEST_ABI trt_glNormalPointer(u32 type, int stride, const void* ptr) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) {
    c->ff_nrm.comps = 3; c->ff_nrm.type = type;
    c->ff_nrm.stride = (u32)stride; c->ff_nrm.ptr = ptr; c->ff_nrm.vbo = c->cur_vbo;
  }
}
void TRT_GUEST_ABI trt_glColorPointer(int size, u32 type, int stride, const void* ptr) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) {
    c->ff_col.comps = (u32)size; c->ff_col.type = type;
    c->ff_col.stride = (u32)stride; c->ff_col.ptr = ptr; c->ff_col.vbo = c->cur_vbo;
  }
}
void TRT_GUEST_ABI trt_glTexCoordPointer(int size, u32 type, int stride, const void* ptr) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) {
    auto& a = c->ff_tc[std::min(c->client_active_tex, 7u)];
    a.comps = (u32)size; a.type = type; a.stride = (u32)stride;
    a.ptr = ptr; a.vbo = c->cur_vbo;
  }
}

void TRT_GUEST_ABI trt_glDrawArrays(u32 mode, int first, int count) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (c && count > 0) emit_array_draw(c, mode, 0, (u32)count, (u32)std::max(0, first), 0, nullptr, 1, 0);
}
void TRT_GUEST_ABI trt_glDrawArraysInstanced(u32 mode, int first, int count, int inst_count) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (c && count > 0 && inst_count > 0)
    emit_array_draw(c, mode, 0, (u32)count, (u32)std::max(0, first), 0, nullptr, (u32)inst_count, 0);
}
void TRT_GUEST_ABI trt_glDrawElements(u32 mode, int count, u32 type, const void* indices) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (c && count > 0) emit_array_draw(c, mode, 1, (u32)count, 0, type, indices, 1, 0);
}
void TRT_GUEST_ABI trt_glDrawElementsInstanced(u32 mode, int count, u32 type,
                                               const void* indices, int inst) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (c && count > 0 && inst > 0)
    emit_array_draw(c, mode, 1, (u32)count, 0, type, indices, (u32)inst, 0);
}
void TRT_GUEST_ABI trt_glDrawElementsBaseVertex(u32 mode, int count, u32 type,
                                                const void* indices, int basev) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (c && count > 0) emit_array_draw(c, mode, 1, (u32)count, 0, type, indices, 1, basev);
}
void TRT_GUEST_ABI trt_glDrawElementsInstancedBaseVertex(u32 mode, int count, u32 type,
                                                         const void* indices, int inst, int basev) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (c && count > 0 && inst > 0)
    emit_array_draw(c, mode, 1, (u32)count, 0, type, indices, (u32)inst, basev);
}
void TRT_GUEST_ABI trt_glDrawRangeElements(u32 mode, u32, u32, int count, u32 type, const void* idx) {
  trt_glDrawElements(mode, count, type, idx);
}
void TRT_GUEST_ABI trt_glMultiDrawArrays(u32 mode, const int* first, const int* count, int dc) {
  for (int i = 0; i < dc; ++i) trt_glDrawArrays(mode, first[i], count[i]);
}
void TRT_GUEST_ABI trt_glMultiDrawElements(u32 mode, const int* count, u32 type,
                                           const void* const* idx, int dc) {
  for (int i = 0; i < dc; ++i) trt_glDrawElements(mode, count[i], type, idx[i]);
}

// ---- Textures & Samplers ---------------------------------------------------
void TRT_GUEST_ABI trt_glActiveTexture(u32 tex) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) {
    c->active_tex_unit = (tex >= 0x84C0u) ? std::min(tex - 0x84C0u, 31u) : 0u;
  }
}
void TRT_GUEST_ABI trt_glGenTextures(int n, u32* textures) {
  if (!textures || n <= 0) return;
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  for (int i = 0; i < n; ++i) {
    u32 id = c->share->next_tex++;
    c->share->texs[id].alive = true;
    textures[i] = id;
  }
}
void TRT_GUEST_ABI trt_glDeleteTextures(int n, const u32* textures) {
  if (!textures || n <= 0) return;
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  for (int i = 0; i < n; ++i) {
    u32 id = textures[i];
    if (!id) continue;
    c->share->texs.erase(id);
    if (g_gl_hooks_set && g_gl_hooks.delete_texture) {
      g_gl_hooks.delete_texture(id, g_gl_hooks.user);
    }
  }
}
u8 TRT_GUEST_ABI trt_glIsTexture(u32 t) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  return (c && c->share->texs.count(t)) ? 1 : 0;
}
void TRT_GUEST_ABI trt_glBindTexture(u32 target, u32 texture) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  if (c->recording_list) {
    c->share->dls[c->recording_list].ops.push_back({DL_OP_BIND_TEX, target, texture, {}, {}});
    if (!c->recording_exec) return;
  }
  u32 u = std::min(c->active_tex_unit, 31u);
  if (texture) c->share->texs[texture].alive = true;
  if (target == GL_TEXTURE_CUBE_MAP_V) c->bound_tex_cube[u] = texture;
  else if (target == GL_TEXTURE_3D_V) c->bound_tex_3d[u] = texture;
  else if (target == GL_TEXTURE_2D_ARRAY_V) c->bound_tex_arr[u] = texture;
  else c->bound_tex_2d[u] = texture;
}

static void upload_tex_sub(wgl_ctx* c, u32 tid, u32 target, int level, int layer,
                           u32 internal_fmt, int w, int h, int depth, u32 fmt,
                           u32 type, const void* pixels, bool is_sub, int xoff, int yoff) {
  gl_tex_obj& tx = c->share->texs[tid];
  tx.alive = true;
  if (target >= GL_TEXTURE_CUBE_MAP_POS_X && target <= GL_TEXTURE_CUBE_MAP_NEG_Z) {
    tx.target = TRT_GL_TARGET_CUBE;
    layer = (int)(target - GL_TEXTURE_CUBE_MAP_POS_X);
  } else if (target == GL_TEXTURE_3D_V) {
    tx.target = TRT_GL_TARGET_3D;
  } else if (target == GL_TEXTURE_2D_ARRAY_V) {
    tx.target = TRT_GL_TARGET_2D_ARRAY;
  } else {
    tx.target = TRT_GL_TARGET_2D;
  }
  if (!is_sub && level == 0) {
    tx.fmt = gl_fmt_to_trt(internal_fmt, fmt, type);
    tx.w = (u32)std::max(1, w);
    tx.h = (u32)std::max(1, h);
    tx.depth = (u32)std::max(1, depth);
  }
  const u8* raw_ptr = (const u8*)pixels;
  if (c->cur_pbo_unpack && !raw_ptr) {
    auto pit = c->share->bufs.find(c->cur_pbo_unpack);
    if (pit != c->share->bufs.end()) raw_ptr = pit->second.data.data();
  } else if (c->cur_pbo_unpack) {
    auto pit = c->share->bufs.find(c->cur_pbo_unpack);
    if (pit != c->share->bufs.end() && (uintptr_t)pixels < pit->second.data.size()) {
      raw_ptr = pit->second.data.data() + (uintptr_t)pixels;
    }
  }
  if (w <= 0 || h <= 0) return;

  c->scratch_tex.clear();
  const void* upload_ptr = nullptr;
  u32 upload_bytes = 0;
  if (raw_ptr) {
    if (tx.fmt == TRT_GL_TEX_BC1 || tx.fmt == TRT_GL_TEX_BC2 || tx.fmt == TRT_GL_TEX_BC3) {
      u32 bx = (u32)((w + 3) / 4), by = (u32)((h + 3) / 4);
      u32 bsz = (tx.fmt == TRT_GL_TEX_BC1) ? 8u : 16u;
      upload_bytes = bx * by * bsz;
      upload_ptr = raw_ptr;
    } else if (tx.fmt == TRT_GL_TEX_RGBA16F) {
      upload_bytes = (u32)w * (u32)h * 8u;
      upload_ptr = raw_ptr;
    } else if (tx.fmt == TRT_GL_TEX_RGBA32F) {
      upload_bytes = (u32)w * (u32)h * 16u;
      upload_ptr = raw_ptr;
    } else if (tx.fmt == TRT_GL_TEX_R32F || tx.fmt == TRT_GL_TEX_D24S8) {
      upload_bytes = (u32)w * (u32)h * 4u;
      upload_ptr = raw_ptr;
    } else {
      convert_pixels_to_rgba8(raw_ptr, w, h, fmt, type, c->unpack_row_len,
                              c->unpack_skip_rows, c->unpack_skip_pixels,
                              c->unpack_align, c->scratch_tex);
      if (is_sub && level == 0 && !tx.level0_rgba.empty() && tx.w > 0 && tx.h > 0) {
        for (int yy = 0; yy < h; ++yy) {
          int dy = yoff + yy;
          if (dy < 0 || dy >= (int)tx.h) continue;
          for (int xx = 0; xx < w; ++xx) {
            int dx = xoff + xx;
            if (dx < 0 || dx >= (int)tx.w) continue;
            memcpy(&tx.level0_rgba[((size_t)dy * tx.w + (size_t)dx) * 4u],
                   &c->scratch_tex[((size_t)yy * (size_t)w + (size_t)xx) * 4u], 4u);
          }
        }
        upload_ptr = tx.level0_rgba.data();
        upload_bytes = (u32)tx.level0_rgba.size();
      } else {
        if (!is_sub && level == 0) tx.level0_rgba = c->scratch_tex;
        upload_ptr = c->scratch_tex.data();
        upload_bytes = (u32)c->scratch_tex.size();
      }
    }
  }
  tx.version++;
  trt_gl_tex_desc desc{};
  desc.target = tx.target; desc.fmt = tx.fmt;
  desc.w = tx.w; desc.h = tx.h; desc.depth = tx.depth;
  desc.mip_count = tx.mip_levels;
  desc.min_filter = tx.min_filter; desc.mag_filter = tx.mag_filter;
  desc.wrap_s = tx.wrap_s; desc.wrap_t = tx.wrap_t; desc.wrap_r = tx.wrap_r;
  desc.compare_mode = tx.compare_mode;
  if (g_gl_hooks_set && g_gl_hooks.sync_texture) {
    g_gl_hooks.sync_texture(tid, &desc, upload_ptr, upload_bytes,
                            (u32)std::max(0, level), (u32)std::max(0, layer),
                            tx.version, g_gl_hooks.user);
  }
}

void TRT_GUEST_ABI trt_glTexImage2D(u32 target, int level, int internalfmt,
                                    int w, int h, int border, u32 fmt,
                                    u32 type, const void* pixels) {
  (void)border;
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  u32 tid = bound_tex_for_target(c, target, c->active_tex_unit);
  if (!tid) return;
  upload_tex_sub(c, tid, target, level, 0, (u32)internalfmt, w, h, 1, fmt, type,
                 pixels, false, 0, 0);
}
void TRT_GUEST_ABI trt_glTexSubImage2D(u32 target, int level, int xoff, int yoff,
                                       int w, int h, u32 fmt, u32 type,
                                       const void* pixels) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  u32 tid = bound_tex_for_target(c, target, c->active_tex_unit);
  if (!tid) return;
  upload_tex_sub(c, tid, target, level, 0, GL_RGBA8_V, w, h, 1, fmt, type,
                 pixels, true, xoff, yoff);
}
void TRT_GUEST_ABI trt_glTexImage1D(u32 target, int level, int internalfmt,
                                    int w, int border, u32 fmt, u32 type,
                                    const void* pixels) {
  (void)target;
  trt_glTexImage2D(GL_TEXTURE_2D_V, level, internalfmt, w, 1, border, fmt, type, pixels);
}
void TRT_GUEST_ABI trt_glTexStorage2D(u32 target, int levels, u32 internalfmt,
                                      int w, int h) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  u32 tid = bound_tex_for_target(c, target, c->active_tex_unit);
  if (!tid) return;
  c->share->texs[tid].mip_levels = (u32)std::max(1, levels);
  upload_tex_sub(c, tid, target, 0, 0, internalfmt, w, h, 1, GL_RGBA_V,
                 GL_UNSIGNED_BYTE_V, nullptr, false, 0, 0);
}
void TRT_GUEST_ABI trt_glCompressedTexImage2D(u32 target, int level, u32 internalfmt,
                                              int w, int h, int, int img_size,
                                              const void* data) {
  (void)img_size;
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  u32 tid = bound_tex_for_target(c, target, c->active_tex_unit);
  if (!tid) return;
  upload_tex_sub(c, tid, target, level, 0, internalfmt, w, h, 1, GL_RGBA_V,
                 GL_UNSIGNED_BYTE_V, data, false, 0, 0);
}
void TRT_GUEST_ABI trt_glGenerateMipmap(u32 target) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  u32 tid = bound_tex_for_target(c, target, c->active_tex_unit);
  if (tid && g_gl_hooks_set && g_gl_hooks.gen_mipmaps) {
    g_gl_hooks.gen_mipmaps(tid, g_gl_hooks.user);
  }
}
void TRT_GUEST_ABI trt_glTexParameteri(u32 target, u32 pname, int param) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  u32 tid = bound_tex_for_target(c, target, c->active_tex_unit);
  if (!tid) return;
  auto& tx = c->share->texs[tid];
  if (pname == GL_TEXTURE_MIN_FILTER_V) tx.min_filter = (u32)param;
  else if (pname == GL_TEXTURE_MAG_FILTER_V) tx.mag_filter = (u32)param;
  else if (pname == GL_TEXTURE_WRAP_S_V) tx.wrap_s = (u32)param;
  else if (pname == GL_TEXTURE_WRAP_T_V) tx.wrap_t = (u32)param;
  else if (pname == GL_TEXTURE_WRAP_R_V) tx.wrap_r = (u32)param;
  else if (pname == GL_TEXTURE_COMPARE_MODE_V) tx.compare_mode = (u32)param;
}
void TRT_GUEST_ABI trt_glTexParameterf(u32 t, u32 p, float v) { trt_glTexParameteri(t, p, (int)v); }
void TRT_GUEST_ABI trt_glTexParameteriv(u32 t, u32 p, const int* v) { if (v) trt_glTexParameteri(t, p, *v); }
void TRT_GUEST_ABI trt_glTexParameterfv(u32 t, u32 p, const float* v) { if (v) trt_glTexParameteri(t, p, (int)*v); }
void TRT_GUEST_ABI trt_glTexEnvi(u32, u32, int) {}
void TRT_GUEST_ABI trt_glTexEnvf(u32, u32, float) {}
void TRT_GUEST_ABI trt_glGetTexLevelParameteriv(u32 target, int, u32 pname, int* params) {
  if (!params) return;
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  u32 tid = c ? bound_tex_for_target(c, target, c->active_tex_unit) : 0;
  if (!tid) { *params = 0; return; }
  const auto& tx = c->share->texs[tid];
  if (pname == 0x1000) *params = (int)tx.w;
  else if (pname == 0x1001) *params = (int)tx.h;
  else *params = 0;
}
void TRT_GUEST_ABI trt_glCopyTexImage2D(u32, int, u32, int, int, int, int, int) {}
void TRT_GUEST_ABI trt_glCopyTexSubImage2D(u32, int, int, int, int, int, int, int) {}
void TRT_GUEST_ABI trt_glGetTexImage(u32 target, int, u32, u32, void* out) {
  if (!out) return;
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  u32 tid = c ? bound_tex_for_target(c, target, c->active_tex_unit) : 0;
  if (tid && !c->share->texs[tid].level0_rgba.empty()) {
    const auto& src = c->share->texs[tid].level0_rgba;
    memcpy(out, src.data(), src.size());
  }
}

void TRT_GUEST_ABI trt_glGenSamplers(int n, u32* s) {
  if (!s || n <= 0) return;
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  for (int i = 0; i < n; ++i) {
    u32 id = c->share->next_samp++;
    c->share->samps[id].alive = true;
    s[i] = id;
  }
}
void TRT_GUEST_ABI trt_glDeleteSamplers(int n, const u32* s) {
  if (!s) return;
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  for (int i = 0; i < n; ++i) c->share->samps.erase(s[i]);
}
void TRT_GUEST_ABI trt_glBindSampler(u32 unit, u32 sampler) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) c->bound_samplers[std::min(unit, 31u)] = sampler;
}
void TRT_GUEST_ABI trt_glSamplerParameteri(u32 sid, u32 pname, int param) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c || !sid) return;
  auto& s = c->share->samps[sid];
  if (pname == GL_TEXTURE_MIN_FILTER_V) s.min_filter = (u32)param;
  else if (pname == GL_TEXTURE_MAG_FILTER_V) s.mag_filter = (u32)param;
  else if (pname == GL_TEXTURE_WRAP_S_V) s.wrap_s = (u32)param;
  else if (pname == GL_TEXTURE_WRAP_T_V) s.wrap_t = (u32)param;
  else if (pname == GL_TEXTURE_COMPARE_MODE_V) s.compare_mode = (u32)param;
}
void TRT_GUEST_ABI trt_glSamplerParameterf(u32 s, u32 p, float v) {
  trt_glSamplerParameteri(s, p, (int)v);
}

// ---- Buffers & VAOs --------------------------------------------------------
void TRT_GUEST_ABI trt_glGenBuffers(int n, u32* buffers) {
  if (!buffers || n <= 0) return;
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  for (int i = 0; i < n; ++i) {
    u32 id = c->share->next_buf++;
    c->share->bufs[id].alive = true;
    buffers[i] = id;
  }
}
void TRT_GUEST_ABI trt_glDeleteBuffers(int n, const u32* buffers) {
  if (!buffers) return;
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  for (int i = 0; i < n; ++i) {
    if (!buffers[i]) continue;
    c->share->bufs.erase(buffers[i]);
    if (g_gl_hooks_set && g_gl_hooks.delete_buffer) {
      g_gl_hooks.delete_buffer(buffers[i], g_gl_hooks.user);
    }
  }
}
u8 TRT_GUEST_ABI trt_glIsBuffer(u32 b) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  return (c && c->share->bufs.count(b)) ? 1 : 0;
}
void TRT_GUEST_ABI trt_glBindBuffer(u32 target, u32 buf) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  if (buf) c->share->bufs[buf].alive = true;
  if (target == GL_ARRAY_BUFFER_V) c->cur_vbo = buf;
  else if (target == GL_ELEMENT_ARRAY_BUFFER_V) get_cur_vao(c)->ibo = buf;
  else if (target == GL_UNIFORM_BUFFER_V) c->cur_ubo = buf;
  else if (target == GL_SHADER_STORAGE_BUFFER_V) c->cur_ssbo = buf;
  else if (target == GL_PIXEL_UNPACK_BUFFER_V) c->cur_pbo_unpack = buf;
}
void TRT_GUEST_ABI trt_glBindBufferBase(u32 target, u32 index, u32 buf) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c || index >= 16) return;
  if (target == GL_UNIFORM_BUFFER_V) {
    c->cur_ubo = buf;
    c->ubo_binds[index] = {buf, 0, 0};
  } else if (target == GL_SHADER_STORAGE_BUFFER_V) {
    c->cur_ssbo = buf;
    c->ssbo_binds[index] = {buf, 0, 0};
  }
}
void TRT_GUEST_ABI trt_glBindBufferRange(u32 target, u32 index, u32 buf,
                                         i64 offset, i64 size) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c || index >= 16) return;
  if (target == GL_UNIFORM_BUFFER_V) {
    c->cur_ubo = buf;
    c->ubo_binds[index] = {buf, (u32)std::max<i64>(0, offset), (u32)std::max<i64>(0, size)};
  } else if (target == GL_SHADER_STORAGE_BUFFER_V) {
    c->cur_ssbo = buf;
    c->ssbo_binds[index] = {buf, (u32)std::max<i64>(0, offset), (u32)std::max<i64>(0, size)};
  }
}
void TRT_GUEST_ABI trt_glBufferData(u32 target, i64 size, const void* data, u32 usage) {
  (void)usage;
  if (size < 0) return;
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  u32 bid = 0;
  gl_buf_obj* b = get_target_buf(c, target, &bid);
  if (!b) return;
  if (target == GL_ELEMENT_ARRAY_BUFFER_V) b->kind_hint = TRT_GL_BUF_INDEX;
  else if (target == GL_UNIFORM_BUFFER_V) b->kind_hint = TRT_GL_BUF_UNIFORM;
  else if (target == GL_SHADER_STORAGE_BUFFER_V) b->kind_hint = TRT_GL_BUF_STORAGE;
  else b->kind_hint = TRT_GL_BUF_VERTEX;
  b->data.resize((size_t)size);
  if (data && size > 0) memcpy(b->data.data(), data, (size_t)size);
  else if (size > 0) memset(b->data.data(), 0, (size_t)size);
  b->dirty = true;
}
void TRT_GUEST_ABI trt_glBufferSubData(u32 target, i64 offset, i64 size, const void* data) {
  if (!data || offset < 0 || size <= 0) return;
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  gl_buf_obj* b = get_target_buf(c, target);
  if (!b || (size_t)(offset + size) > b->data.size()) return;
  memcpy(b->data.data() + offset, data, (size_t)size);
  b->dirty = true;
}
void TRT_GUEST_ABI trt_glGetBufferSubData(u32 target, i64 offset, i64 size, void* data) {
  if (!data || offset < 0 || size <= 0) return;
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  gl_buf_obj* b = c ? get_target_buf(c, target) : nullptr;
  if (!b || (size_t)(offset + size) > b->data.size()) return;
  memcpy(data, b->data.data() + offset, (size_t)size);
}
void* TRT_GUEST_ABI trt_glMapBufferRange(u32 target, i64 offset, i64 length, u32 access) {
  if (offset < 0 || length < 0) return nullptr;
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  gl_buf_obj* b = c ? get_target_buf(c, target) : nullptr;
  if (!b || (size_t)(offset + length) > b->data.size()) return nullptr;
  b->mapped = true; b->map_off = (u64)offset; b->map_len = (u64)length; b->map_access = access;
  return b->data.data() + offset;
}
void* TRT_GUEST_ABI trt_glMapBuffer(u32 target, u32 access) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  gl_buf_obj* b = c ? get_target_buf(c, target) : nullptr;
  if (!b) return nullptr;
  b->mapped = true; b->map_off = 0; b->map_len = b->data.size(); b->map_access = access;
  return b->data.data();
}
void TRT_GUEST_ABI trt_glFlushMappedBufferRange(u32 target, i64, i64) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) {
    if (gl_buf_obj* b = get_target_buf(c, target)) b->dirty = true;
  }
}
u8 TRT_GUEST_ABI trt_glUnmapBuffer(u32 target) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  gl_buf_obj* b = c ? get_target_buf(c, target) : nullptr;
  if (!b) return 0;
  b->mapped = false;
  b->dirty = true;
  return 1;
}
void TRT_GUEST_ABI trt_glGetBufferParameteriv(u32 target, u32 pname, int* params) {
  if (!params) return;
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  gl_buf_obj* b = c ? get_target_buf(c, target) : nullptr;
  if (!b) { *params = 0; return; }
  if (pname == 0x8764) *params = (int)b->data.size(); // GL_BUFFER_SIZE
  else *params = 0;
}

void TRT_GUEST_ABI trt_glGenVertexArrays(int n, u32* arrays) {
  if (!arrays || n <= 0) return;
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  for (int i = 0; i < n; ++i) {
    u32 id = c->next_vao++;
    c->vaos[id].alive = true;
    arrays[i] = id;
  }
}
void TRT_GUEST_ABI trt_glDeleteVertexArrays(int n, const u32* arrays) {
  if (!arrays) return;
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  for (int i = 0; i < n; ++i) if (arrays[i]) c->vaos.erase(arrays[i]);
}
void TRT_GUEST_ABI trt_glBindVertexArray(u32 arr) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  c->cur_vao = arr;
  c->vaos[arr].alive = true;
}
u8 TRT_GUEST_ABI trt_glIsVertexArray(u32 arr) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  return (c && c->vaos.count(arr)) ? 1 : 0;
}
void TRT_GUEST_ABI trt_glEnableVertexAttribArray(u32 idx) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) {
    if (idx < 16) get_cur_vao(c)->attribs[idx].enabled = 1;
  }
}
void TRT_GUEST_ABI trt_glDisableVertexAttribArray(u32 idx) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) {
    if (idx < 16) get_cur_vao(c)->attribs[idx].enabled = 0;
  }
}
void TRT_GUEST_ABI trt_glVertexAttribPointer(u32 idx, int size, u32 type, u8 norm,
                                             int stride, const void* ptr) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c || idx >= 16) return;
  auto* vao = get_cur_vao(c);
  auto& ab = vao->attribs[idx];
  ab.location = idx;
  ab.vbo_gl = c->cur_vbo;
  ab.components = (u32)size;
  ab.gl_type = type;
  ab.normalized = norm ? 1 : 0;
  ab.is_int = 0;
  ab.stride = stride > 0 ? (u32)stride : ((u32)size * gl_type_bytes(type));
  ab.offset = (u32)(uintptr_t)ptr;
  vao->ptrs[idx] = ptr;
}
void TRT_GUEST_ABI trt_glVertexAttribIPointer(u32 idx, int size, u32 type,
                                              int stride, const void* ptr) {
  trt_glVertexAttribPointer(idx, size, type, 0, stride, ptr);
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) {
    if (idx < 16) get_cur_vao(c)->attribs[idx].is_int = 1;
  }
}
void TRT_GUEST_ABI trt_glVertexAttribDivisor(u32 idx, u32 div) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) {
    if (idx < 16) get_cur_vao(c)->attribs[idx].divisor = (u8)std::min(div, 250u);
  }
}
void TRT_GUEST_ABI trt_glVertexAttrib4f(u32 idx, float x, float y, float z, float w) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) {
    if (idx < 16) {
      c->cur_attrib[idx][0] = x; c->cur_attrib[idx][1] = y;
      c->cur_attrib[idx][2] = z; c->cur_attrib[idx][3] = w;
    }
  }
}
void TRT_GUEST_ABI trt_glVertexAttrib3f(u32 i, float x, float y, float z) { trt_glVertexAttrib4f(i, x, y, z, 1); }
void TRT_GUEST_ABI trt_glVertexAttrib2f(u32 i, float x, float y) { trt_glVertexAttrib4f(i, x, y, 0, 1); }
void TRT_GUEST_ABI trt_glVertexAttrib1f(u32 i, float x) { trt_glVertexAttrib4f(i, x, 0, 0, 1); }
void TRT_GUEST_ABI trt_glVertexAttrib4fv(u32 i, const float* v) {
  if (v) trt_glVertexAttrib4f(i, v[0], v[1], v[2], v[3]);
}

// ---- Shaders & Programs ----------------------------------------------------
u32 TRT_GUEST_ABI trt_glCreateShader(u32 shader_type) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return 0;
  u32 id = c->share->next_sh++;
  auto& s = c->share->shaders[id];
  s.alive = true; s.type = shader_type;
  return id;
}
void TRT_GUEST_ABI trt_glDeleteShader(u32 sh) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c || !sh) return;
  c->share->shaders.erase(sh);
}
void TRT_GUEST_ABI trt_glShaderSource(u32 sh, int count, const char* const* str,
                                      const int* length) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c || !str || count <= 0) return;
  auto it = c->share->shaders.find(sh);
  if (it == c->share->shaders.end()) return;
  it->second.source.clear();
  for (int i = 0; i < count; ++i) {
    if (!str[i]) continue;
    if (length && length[i] >= 0) it->second.source.append(str[i], (size_t)length[i]);
    else it->second.source.append(str[i]);
  }
}
void TRT_GUEST_ABI trt_glCompileShader(u32 sh) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  auto it = c->share->shaders.find(sh);
  if (it == c->share->shaders.end()) return;
  glsl_stage st = GLSL_STAGE_VERTEX;
  if (it->second.type == GL_FRAGMENT_SHADER_V) st = GLSL_STAGE_FRAGMENT;
  else if (it->second.type == GL_COMPUTE_SHADER_V) st = GLSL_STAGE_COMPUTE;
  glsl_compile_result r = glsl_to_hlsl(it->second.source.c_str(),
                                       (u32)it->second.source.size(), st, nullptr);
  it->second.compiled = r.ok;
  it->second.log = r.log;
}
void TRT_GUEST_ABI trt_glGetShaderiv(u32 sh, u32 pname, int* params) {
  if (!params) return;
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) { *params = 0; return; }
  auto it = c->share->shaders.find(sh);
  if (it == c->share->shaders.end()) { *params = 0; return; }
  if (pname == GL_COMPILE_STATUS_V) *params = it->second.compiled ? 1 : 0;
  else if (pname == GL_SHADER_TYPE_V) *params = (int)it->second.type;
  else if (pname == GL_INFO_LOG_LENGTH_V) *params = (int)it->second.log.size() + 1;
  else if (pname == GL_SHADER_SOURCE_LENGTH_V) *params = (int)it->second.source.size() + 1;
  else *params = 0;
}
void TRT_GUEST_ABI trt_glGetShaderInfoLog(u32 sh, int max_len, int* length, char* info) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  std::string msg;
  if (c) {
    auto it = c->share->shaders.find(sh);
    if (it != c->share->shaders.end()) msg = it->second.log;
  }
  int cp = (max_len > 0) ? (int)std::min<size_t>(msg.size(), (size_t)(max_len - 1)) : 0;
  if (info && max_len > 0) { memcpy(info, msg.data(), (size_t)cp); info[cp] = 0; }
  if (length) *length = cp;
}
u8 TRT_GUEST_ABI trt_glIsShader(u32 sh) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  return (c && c->share->shaders.count(sh)) ? 1 : 0;
}

u32 TRT_GUEST_ABI trt_glCreateProgram(void) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return 0;
  u32 id = c->share->next_prog++;
  c->share->progs[id].alive = true;
  return id;
}
void TRT_GUEST_ABI trt_glDeleteProgram(u32 prog) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c || !prog) return;
  if (c->cur_prog == prog) c->cur_prog = 0;
  c->share->progs.erase(prog);
  if (g_gl_hooks_set && g_gl_hooks.delete_program) {
    g_gl_hooks.delete_program(prog, g_gl_hooks.user);
  }
}
void TRT_GUEST_ABI trt_glAttachShader(u32 prog, u32 sh) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  auto it = c->share->progs.find(prog);
  if (it != c->share->progs.end()) it->second.attached.push_back(sh);
}
void TRT_GUEST_ABI trt_glDetachShader(u32 prog, u32 sh) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  auto it = c->share->progs.find(prog);
  if (it == c->share->progs.end()) return;
  auto& a = it->second.attached;
  a.erase(std::remove(a.begin(), a.end(), sh), a.end());
}
void TRT_GUEST_ABI trt_glBindAttribLocation(u32 prog, u32 index, const char* name) {
  if (!name) return;
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (c && c->share->progs.count(prog)) {
    c->share->progs[prog].explicit_attrib_locs[name] = (int)index;
  }
}
void TRT_GUEST_ABI trt_glBindFragDataLocation(u32 prog, u32 col, const char* name) {
  if (!name) return;
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (c && c->share->progs.count(prog)) {
    c->share->progs[prog].explicit_frag_locs[name] = (int)col;
  }
}
void TRT_GUEST_ABI trt_glLinkProgram(u32 prog) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  auto pit = c->share->progs.find(prog);
  if (pit == c->share->progs.end()) return;
  gl_prog_obj& p = pit->second;

  glsl_link_bindings link{};
  for (const auto& kv : p.explicit_attrib_locs) link.attrib_locs.push_back({kv.first.c_str(), kv.second});
  for (const auto& kv : p.explicit_frag_locs) link.frag_locs.push_back({kv.first.c_str(), kv.second});

  p.vs_res = {}; p.ps_res = {}; p.cs_res = {};
  const gl_shader_obj *vso = nullptr, *pso = nullptr, *cso = nullptr;
  for (u32 sid : p.attached) {
    auto sit = c->share->shaders.find(sid);
    if (sit == c->share->shaders.end()) continue;
    if (sit->second.type == GL_VERTEX_SHADER_V) vso = &sit->second;
    else if (sit->second.type == GL_FRAGMENT_SHADER_V) pso = &sit->second;
    else if (sit->second.type == GL_COMPUTE_SHADER_V) cso = &sit->second;
  }
  p.log.clear();
  if (cso) {
    p.cs_res = glsl_to_hlsl(cso->source.c_str(), (u32)cso->source.size(), GLSL_STAGE_COMPUTE, &link);
    p.linked = p.cs_res.ok;
    p.log += p.cs_res.log;
  } else {
    if (vso) {
      p.vs_res = glsl_to_hlsl(vso->source.c_str(), (u32)vso->source.size(), GLSL_STAGE_VERTEX, &link);
      for (const auto& ov : p.vs_res.outputs) {
        if (!ov.is_builtin) link.varying_locs.push_back({ov.name.c_str(), ov.location});
      }
      p.log += p.vs_res.log;
    }
    if (pso) {
      p.ps_res = glsl_to_hlsl(pso->source.c_str(), (u32)pso->source.size(), GLSL_STAGE_FRAGMENT, &link);
      p.log += p.ps_res.log;
    }
    p.linked = (!vso || p.vs_res.ok) && (!pso || p.ps_res.ok);
  }
  populate_program_reflection(p);
  if (p.linked && g_gl_hooks_set && g_gl_hooks.compile_program) {
    u32 hr = g_gl_hooks.compile_program(
        prog,
        p.vs_res.ok ? p.vs_res.hlsl.c_str() : nullptr, p.vs_res.ok ? (u32)p.vs_res.hlsl.size() : 0,
        p.ps_res.ok ? p.ps_res.hlsl.c_str() : nullptr, p.ps_res.ok ? (u32)p.ps_res.hlsl.size() : 0,
        p.cs_res.ok ? p.cs_res.hlsl.c_str() : nullptr, p.cs_res.ok ? (u32)p.cs_res.hlsl.size() : 0,
        g_gl_hooks.user);
    if (!hr) {
      p.linked = false;
      p.log += "\nD3DCompile backend failed to compile translated HLSL.";
    }
  }
}
void TRT_GUEST_ABI trt_glValidateProgram(u32) {}
void TRT_GUEST_ABI trt_glUseProgram(u32 prog) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) c->cur_prog = prog;
}
void TRT_GUEST_ABI trt_glGetProgramiv(u32 prog, u32 pname, int* params) {
  if (!params) return;
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) { *params = 0; return; }
  auto it = c->share->progs.find(prog);
  if (it == c->share->progs.end()) { *params = 0; return; }
  const gl_prog_obj& p = it->second;
  if (pname == GL_LINK_STATUS_V) *params = p.linked ? 1 : 0;
  else if (pname == GL_VALIDATE_STATUS_V) *params = p.linked ? 1 : 0;
  else if (pname == GL_INFO_LOG_LENGTH_V) *params = (int)p.log.size() + 1;
  else if (pname == GL_ATTACHED_SHADERS_V) *params = (int)p.attached.size();
  else if (pname == GL_ACTIVE_UNIFORMS_V) *params = (int)p.uniforms.size();
  else if (pname == GL_ACTIVE_ATTRIBUTES_V) *params = (int)p.vs_res.inputs.size();
  else if (pname == GL_ACTIVE_UNIFORM_BLOCKS_V) *params = (int)p.ubos.size();
  else if (pname == GL_ACTIVE_UNIFORM_MAX_LENGTH_V || pname == GL_ACTIVE_ATTRIBUTE_MAX_LENGTH_V) *params = 128;
  else *params = 0;
}
void TRT_GUEST_ABI trt_glGetProgramInfoLog(u32 prog, int max_len, int* len, char* info) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  std::string msg;
  if (c && c->share->progs.count(prog)) msg = c->share->progs[prog].log;
  int cp = (max_len > 0) ? (int)std::min<size_t>(msg.size(), (size_t)(max_len - 1)) : 0;
  if (info && max_len > 0) { memcpy(info, msg.data(), (size_t)cp); info[cp] = 0; }
  if (len) *len = cp;
}
u8 TRT_GUEST_ABI trt_glIsProgram(u32 p) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  return (c && c->share->progs.count(p)) ? 1 : 0;
}
int TRT_GUEST_ABI trt_glGetAttribLocation(u32 prog, const char* name) {
  if (!name) return -1;
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c || !c->share->progs.count(prog)) return -1;
  for (const auto& iv : c->share->progs[prog].vs_res.inputs) {
    if (iv.name == name) return iv.location;
  }
  return -1;
}
int TRT_GUEST_ABI trt_glGetUniformLocation(u32 prog, const char* name) {
  if (!name) return -1;
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c || !c->share->progs.count(prog)) return -1;
  const gl_prog_obj& p = c->share->progs[prog];
  std::string key(name);
  u32 elem = 0;
  size_t lb = key.find('[');
  if (lb != std::string::npos) {
    elem = (u32)atoi(key.c_str() + lb + 1);
    key.resize(lb);
  }
  auto it = p.uniform_name_to_idx.find(key);
  if (it == p.uniform_name_to_idx.end()) return -1;
  return (int)((it->second << 12) | (elem & 0xFFFu));
}
void TRT_GUEST_ABI trt_glGetActiveUniform(u32 prog, u32 idx, int buf_sz, int* len,
                                          int* size, u32* type, char* name) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c || !c->share->progs.count(prog)) return;
  const auto& p = c->share->progs[prog];
  if (idx >= p.uniforms.size()) return;
  const auto& u = p.uniforms[idx];
  if (size) *size = (int)u.array_len;
  if (type) *type = GL_FLOAT_V;
  int cp = (buf_sz > 0) ? (int)std::min<size_t>(u.name.size(), (size_t)(buf_sz - 1)) : 0;
  if (name && buf_sz > 0) { memcpy(name, u.name.data(), (size_t)cp); name[cp] = 0; }
  if (len) *len = cp;
}
void TRT_GUEST_ABI trt_glGetActiveAttrib(u32 prog, u32 idx, int buf_sz, int* len,
                                         int* size, u32* type, char* name) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c || !c->share->progs.count(prog)) return;
  const auto& ins = c->share->progs[prog].vs_res.inputs;
  if (idx >= ins.size()) return;
  if (size) *size = 1;
  if (type) *type = GL_FLOAT_V;
  int cp = (buf_sz > 0) ? (int)std::min<size_t>(ins[idx].name.size(), (size_t)(buf_sz - 1)) : 0;
  if (name && buf_sz > 0) { memcpy(name, ins[idx].name.data(), (size_t)cp); name[cp] = 0; }
  if (len) *len = cp;
}
u32 TRT_GUEST_ABI trt_glGetUniformBlockIndex(u32 prog, const char* name) {
  if (!name) return 0xFFFFFFFFu;
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c || !c->share->progs.count(prog)) return 0xFFFFFFFFu;
  const auto& ubs = c->share->progs[prog].ubos;
  for (size_t i = 0; i < ubs.size(); ++i) if (ubs[i].name == name) return (u32)i;
  return 0xFFFFFFFFu;
}
void TRT_GUEST_ABI trt_glUniformBlockBinding(u32 prog, u32 idx, u32 binding) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c || !c->share->progs.count(prog)) return;
  auto& ubs = c->share->progs[prog].ubos;
  if (idx < ubs.size()) ubs[idx].block_binding = binding;
}

static void set_unif_bytes(int loc, const void* data, u32 bytes) {
  if (loc < 0) return;
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c || !c->cur_prog) return;
  auto it = c->share->progs.find(c->cur_prog);
  if (it == c->share->progs.end()) return;
  u32 uidx = (u32)loc >> 12;
  u32 elem = (u32)loc & 0xFFFu;
  write_prog_cb_bytes(it->second, uidx, elem, data, bytes);
}
void TRT_GUEST_ABI trt_glUniform1i(int loc, int v0) {
  if (loc < 0) return;
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c || !c->cur_prog) return;
  auto it = c->share->progs.find(c->cur_prog);
  if (it == c->share->progs.end()) return;
  u32 uidx = (u32)loc >> 12;
  u32 elem = (u32)loc & 0xFFFu;
  if (uidx < it->second.uniforms.size()) {
    auto& u = it->second.uniforms[uidx];
    if (u.is_sampler || u.is_image) { u.sampler_unit = v0; return; }
  }
  write_prog_cb_bytes(it->second, uidx, elem, &v0, 4);
}
void TRT_GUEST_ABI trt_glUniform2i(int l, int a, int b) { int v[2] = {a, b}; set_unif_bytes(l, v, 8); }
void TRT_GUEST_ABI trt_glUniform3i(int l, int a, int b, int c) { int v[3] = {a, b, c}; set_unif_bytes(l, v, 12); }
void TRT_GUEST_ABI trt_glUniform4i(int l, int a, int b, int c, int d) { int v[4] = {a, b, c, d}; set_unif_bytes(l, v, 16); }
void TRT_GUEST_ABI trt_glUniform1iv(int l, int cnt, const int* v) {
  if (v && cnt > 0) for (int i = 0; i < cnt; ++i) trt_glUniform1i(l + i, v[i]);
}
void TRT_GUEST_ABI trt_glUniform1f(int l, float a) { set_unif_bytes(l, &a, 4); }
void TRT_GUEST_ABI trt_glUniform2f(int l, float a, float b) { float v[2] = {a, b}; set_unif_bytes(l, v, 8); }
void TRT_GUEST_ABI trt_glUniform3f(int l, float a, float b, float c) { float v[3] = {a, b, c}; set_unif_bytes(l, v, 12); }
void TRT_GUEST_ABI trt_glUniform4f(int l, float a, float b, float c, float d) { float v[4] = {a, b, c, d}; set_unif_bytes(l, v, 16); }
void TRT_GUEST_ABI trt_glUniform1fv(int l, int cnt, const float* v) {
  if (v && cnt > 0) for (int i = 0; i < cnt; ++i) set_unif_bytes(l + i, &v[i], 4);
}
void TRT_GUEST_ABI trt_glUniform2fv(int l, int cnt, const float* v) {
  if (v && cnt > 0) for (int i = 0; i < cnt; ++i) set_unif_bytes(l + i, v + i * 2, 8);
}
void TRT_GUEST_ABI trt_glUniform3fv(int l, int cnt, const float* v) {
  if (v && cnt > 0) for (int i = 0; i < cnt; ++i) set_unif_bytes(l + i, v + i * 3, 12);
}
void TRT_GUEST_ABI trt_glUniform4fv(int l, int cnt, const float* v) {
  if (v && cnt > 0) for (int i = 0; i < cnt; ++i) set_unif_bytes(l + i, v + i * 4, 16);
}
void TRT_GUEST_ABI trt_glUniformMatrix4fv(int l, int cnt, u8 transp, const float* v) {
  if (!v || cnt <= 0 || l < 0) return;
  for (int i = 0; i < cnt; ++i) {
    float buf[16];
    const float* m = v + i * 16;
    if (transp) {
      for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) buf[c * 4 + r] = m[r * 4 + c];
      m = buf;
    }
    set_unif_bytes(l + i, m, 64);
  }
}
void TRT_GUEST_ABI trt_glUniformMatrix3fv(int l, int cnt, u8 transp, const float* v) {
  if (!v || cnt <= 0 || l < 0) return;
  for (int i = 0; i < cnt; ++i) {
    float buf[12] = {};
    const float* m = v + i * 9;
    for (int col = 0; col < 3; ++col) {
      for (int row = 0; row < 3; ++row) {
        buf[col * 4 + row] = transp ? m[row * 3 + col] : m[col * 3 + row];
      }
    }
    set_unif_bytes(l + i, buf, 48);
  }
}
void TRT_GUEST_ABI trt_glUniformMatrix2fv(int l, int cnt, u8 transp, const float* v) {
  if (!v || cnt <= 0 || l < 0) return;
  for (int i = 0; i < cnt; ++i) {
    float buf[8] = {};
    const float* m = v + i * 4;
    for (int col = 0; col < 2; ++col) {
      for (int row = 0; row < 2; ++row) {
        buf[col * 4 + row] = transp ? m[row * 2 + col] : m[col * 2 + row];
      }
    }
    set_unif_bytes(l + i, buf, 32);
  }
}

// ---- Framebuffers, Renderbuffers, Compute ----------------------------------
void TRT_GUEST_ABI trt_glGenFramebuffers(int n, u32* fbos) {
  if (!fbos || n <= 0) return;
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  for (int i = 0; i < n; ++i) {
    u32 id = c->next_fbo++;
    c->fbos[id].alive = true;
    fbos[i] = id;
  }
}
void TRT_GUEST_ABI trt_glDeleteFramebuffers(int n, const u32* fbos) {
  if (!fbos) return;
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  for (int i = 0; i < n; ++i) {
    if (!fbos[i]) continue;
    if (c->cur_draw_fbo == fbos[i]) c->cur_draw_fbo = 0;
    if (c->cur_read_fbo == fbos[i]) c->cur_read_fbo = 0;
    c->fbos.erase(fbos[i]);
    if (g_gl_hooks_set && g_gl_hooks.delete_fbo) {
      g_gl_hooks.delete_fbo(fbos[i], g_gl_hooks.user);
    }
  }
}
void TRT_GUEST_ABI trt_glBindFramebuffer(u32 target, u32 fbo) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  c->fbos[fbo].alive = true;
  if (target == GL_FRAMEBUFFER_V || target == GL_DRAW_FRAMEBUFFER_V) c->cur_draw_fbo = fbo;
  if (target == GL_FRAMEBUFFER_V || target == GL_READ_FRAMEBUFFER_V) c->cur_read_fbo = fbo;
}
u32 TRT_GUEST_ABI trt_glCheckFramebufferStatus(u32) { return GL_FRAMEBUFFER_COMPLETE_V; }
void TRT_GUEST_ABI trt_glFramebufferTexture2D(u32 target, u32 attach, u32 textarget,
                                              u32 tex, int level) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  u32 fbo_id = (target == GL_READ_FRAMEBUFFER_V) ? c->cur_read_fbo : c->cur_draw_fbo;
  if (!fbo_id) return;
  auto& f = c->fbos[fbo_id];
  u32 layer = (textarget >= GL_TEXTURE_CUBE_MAP_POS_X && textarget <= GL_TEXTURE_CUBE_MAP_NEG_Z)
                  ? (textarget - GL_TEXTURE_CUBE_MAP_POS_X) : 0u;
  if (attach >= GL_COLOR_ATTACHMENT0_V && attach < GL_COLOR_ATTACHMENT0_V + 8u) {
    u32 slot = attach - GL_COLOR_ATTACHMENT0_V;
    f.color_tex[slot] = tex;
    f.color_mip[slot] = (u32)std::max(0, level);
    f.color_layer[slot] = layer;
    if (slot + 1 > f.draw_buf_count) f.draw_buf_count = slot + 1;
  } else if (attach == GL_DEPTH_ATTACHMENT_V || attach == GL_DEPTH_STENCIL_ATTACHMENT_V) {
    f.depth_tex = tex;
    if (attach == GL_DEPTH_STENCIL_ATTACHMENT_V) f.stencil_tex = tex;
  } else if (attach == GL_STENCIL_ATTACHMENT_V) {
    f.stencil_tex = tex;
  }
  f.dirty = true;
}
void TRT_GUEST_ABI trt_glFramebufferTexture(u32 target, u32 attach, u32 tex, int lvl) {
  trt_glFramebufferTexture2D(target, attach, GL_TEXTURE_2D_V, tex, lvl);
}
void TRT_GUEST_ABI trt_glGenRenderbuffers(int n, u32* rbos) {
  if (!rbos || n <= 0) return;
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  for (int i = 0; i < n; ++i) {
    u32 id = c->share->next_rbo++;
    auto& r = c->share->rbos[id];
    r.alive = true;
    r.backing_tex = c->share->next_tex++;
    c->share->texs[r.backing_tex].alive = true;
    rbos[i] = id;
  }
}
void TRT_GUEST_ABI trt_glDeleteRenderbuffers(int n, const u32* rbos) {
  if (!rbos) return;
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  for (int i = 0; i < n; ++i) {
    auto it = c->share->rbos.find(rbos[i]);
    if (it != c->share->rbos.end()) {
      u32 bt = it->second.backing_tex;
      c->share->rbos.erase(it);
      if (bt && g_gl_hooks_set && g_gl_hooks.delete_texture) {
        g_gl_hooks.delete_texture(bt, g_gl_hooks.user);
      }
    }
  }
}
void TRT_GUEST_ABI trt_glBindRenderbuffer(u32, u32 rbo) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) c->cur_rbo = rbo;
}
void TRT_GUEST_ABI trt_glRenderbufferStorage(u32, u32 internalfmt, int w, int h) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c || !c->cur_rbo) return;
  auto it = c->share->rbos.find(c->cur_rbo);
  if (it == c->share->rbos.end()) return;
  it->second.w = (u32)w; it->second.h = (u32)h;
  it->second.fmt = gl_fmt_to_trt(internalfmt, GL_RGBA_V, GL_UNSIGNED_BYTE_V);
  upload_tex_sub(c, it->second.backing_tex, GL_TEXTURE_2D_V, 0, 0, internalfmt,
                 w, h, 1, GL_RGBA_V, GL_UNSIGNED_BYTE_V, nullptr, false, 0, 0);
}
void TRT_GUEST_ABI trt_glRenderbufferStorageMultisample(u32 t, int, u32 fmt, int w, int h) {
  trt_glRenderbufferStorage(t, fmt, w, h);
}
void TRT_GUEST_ABI trt_glFramebufferRenderbuffer(u32 target, u32 attach, u32, u32 rbo) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c) return;
  u32 bt = 0;
  if (rbo && c->share->rbos.count(rbo)) bt = c->share->rbos[rbo].backing_tex;
  u32 fbo_id = (target == GL_READ_FRAMEBUFFER_V) ? c->cur_read_fbo : c->cur_draw_fbo;
  if (!fbo_id) return;
  auto& f = c->fbos[fbo_id];
  if (attach >= GL_COLOR_ATTACHMENT0_V && attach < GL_COLOR_ATTACHMENT0_V + 8u) {
    f.color_tex[attach - GL_COLOR_ATTACHMENT0_V] = bt;
  } else if (attach == GL_DEPTH_ATTACHMENT_V || attach == GL_DEPTH_STENCIL_ATTACHMENT_V) {
    f.depth_tex = bt;
    if (attach == GL_DEPTH_STENCIL_ATTACHMENT_V) f.stencil_tex = bt;
  }
  f.dirty = true;
}
void TRT_GUEST_ABI trt_glBlitFramebuffer(int, int, int, int, int, int, int, int, u32, u32) {}

void TRT_GUEST_ABI trt_glBindImageTexture(u32 unit, u32 tex, int, u8, int, u32, u32) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) if (unit < 8u) c->bound_image[unit] = tex;
}
void TRT_GUEST_ABI trt_glMemoryBarrier(u32) {}
void TRT_GUEST_ABI trt_glDispatchCompute(u32 gx, u32 gy, u32 gz) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c || !c->cur_prog) return;
  auto it = c->share->progs.find(c->cur_prog);
  if (it == c->share->progs.end() || !it->second.linked || !it->second.cs_res.ok) return;
  if (g_gl_hooks_set && g_gl_hooks.dispatch_compute) {
    trt_gl_sampler_slot texs[16] = {};
    trt_gl_ubo_slot ubos[14] = {};
    u32 tc = 0, uc = 0;
    for (const auto& u : it->second.uniforms) {
      if (u.is_sampler && u.cs_tex >= 0 && u.cs_tex < 16) {
        u32 unit = (u32)std::max(0, std::min(31, u.sampler_unit));
        texs[u.cs_tex].gl_tex = c->bound_tex_2d[unit];
        tc = std::max(tc, (u32)u.cs_tex + 1u);
      }
    }
    for (const auto& ub : it->second.ubos) {
      if (ub.slot < 14 && ub.block_binding < 16 && c->ubo_binds[ub.block_binding].buf) {
        const auto& b = c->ubo_binds[ub.block_binding];
        auto bit = c->share->bufs.find(b.buf);
        if (bit != c->share->bufs.end()) sync_buf_to_host(b.buf, bit->second);
        ubos[ub.slot] = {b.buf, b.off, b.sz};
        uc = std::max(uc, ub.slot + 1u);
      }
    }
    g_gl_hooks.dispatch_compute(
        c->cur_prog, gx, gy, gz,
        it->second.cs_cb0.data(), (u32)it->second.cs_cb0.size(),
        texs, tc, ubos, uc, c->bound_image, 8, g_gl_hooks.user);
  }
}

void* TRT_GUEST_ABI trt_glFenceSync(u32, u32) { return (void*)(uintptr_t)0x51510001u; }
void TRT_GUEST_ABI trt_glDeleteSync(void*) {}
u32 TRT_GUEST_ABI trt_glClientWaitSync(void*, u32, u64) { return GL_ALREADY_SIGNALED_V; }
void TRT_GUEST_ABI trt_glWaitSync(void*, u32, u64) {}
void TRT_GUEST_ABI trt_glGetSynciv(void*, u32 pname, int, int* len, int* val) {
  if (val) *val = (pname == GL_SYNC_STATUS_V) ? (int)GL_SIGNALED_V : 0;
  if (len) *len = 1;
}
void TRT_GUEST_ABI trt_glGenQueries(int n, u32* ids) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  wgl_ctx* c = cur_ctx();
  if (!c || !ids) return;
  for (int i = 0; i < n; ++i) ids[i] = c->next_query++;
}
void TRT_GUEST_ABI trt_glDeleteQueries(int, const u32*) {}
void TRT_GUEST_ABI trt_glBeginQuery(u32, u32) {}
void TRT_GUEST_ABI trt_glEndQuery(u32) {}
void TRT_GUEST_ABI trt_glGetQueryObjectiv(u32, u32 pname, int* params) {
  if (params) *params = (pname == GL_QUERY_RESULT_AVAILABLE_V) ? 1 : 0;
}
void TRT_GUEST_ABI trt_glGetQueryObjectuiv(u32 q, u32 p, u32* params) {
  int v = 0;
  trt_glGetQueryObjectiv(q, p, &v);
  if (params) *params = (u32)v;
}
void TRT_GUEST_ABI trt_glDebugMessageCallback(void*, void*) {}
void TRT_GUEST_ABI trt_glDebugMessageControl(u32, u32, u32, int, const u32*, u8) {}

// ---- Full procedure lookup table (exports + wglGetProcAddress) -------------
const gl_proc_entry k_all_gl_procs[] = {
    {"wglCreateContext", (void*)&trt_wglCreateContext},
    {"wglCreateLayerContext", (void*)&trt_wglCreateLayerContext},
    {"wglCreateContextAttribsARB", (void*)&trt_wglCreateContextAttribsARB},
    {"wglDeleteContext", (void*)&trt_wglDeleteContext},
    {"wglMakeCurrent", (void*)&trt_wglMakeCurrent},
    {"wglGetCurrentContext", (void*)&trt_wglGetCurrentContext},
    {"wglGetCurrentDC", (void*)&trt_wglGetCurrentDC},
    {"wglShareLists", (void*)&trt_wglShareLists},
    {"wglCopyContext", (void*)&trt_wglCopyContext},
    {"wglUseFontBitmapsA", (void*)&trt_wglUseFontBitmapsA},
    {"wglUseFontBitmapsW", (void*)&trt_wglUseFontBitmapsW},
    {"wglSwapLayerBuffers", (void*)&trt_wglSwapLayerBuffers},
    {"wglChoosePixelFormat", (void*)&trt_wglChoosePixelFormat},
    {"wglSetPixelFormat", (void*)&trt_wglSetPixelFormat},
    {"wglGetPixelFormat", (void*)&trt_wglGetPixelFormat},
    {"wglDescribePixelFormat", (void*)&trt_wglDescribePixelFormat},
    {"wglGetExtensionsStringARB", (void*)&trt_wglGetExtensionsStringARB},
    {"wglGetExtensionsStringEXT", (void*)&trt_wglGetExtensionsStringEXT},
    {"wglChoosePixelFormatARB", (void*)&trt_wglChoosePixelFormatARB},
    {"wglGetPixelFormatAttribivARB", (void*)&trt_wglGetPixelFormatAttribivARB},
    {"wglSwapIntervalEXT", (void*)&trt_wglSwapIntervalEXT},
    {"wglGetSwapIntervalEXT", (void*)&trt_wglGetSwapIntervalEXT},
    {"wglGetProcAddress", (void*)&trt_wglGetProcAddress},

    {"glGetError", (void*)&trt_glGetError},
    {"glGetString", (void*)&trt_glGetString},
    {"glGetStringi", (void*)&trt_glGetStringi},
    {"glGetIntegerv", (void*)&trt_glGetIntegerv},
    {"glGetInteger64v", (void*)&trt_glGetInteger64v},
    {"glGetIntegeri_v", (void*)&trt_glGetIntegeri_v},
    {"glGetBooleanv", (void*)&trt_glGetBooleanv},
    {"glGetFloatv", (void*)&trt_glGetFloatv},
    {"glGetDoublev", (void*)&trt_glGetDoublev},
    {"glEnable", (void*)&trt_glEnable},
    {"glDisable", (void*)&trt_glDisable},
    {"glIsEnabled", (void*)&trt_glIsEnabled},
    {"glViewport", (void*)&trt_glViewport},
    {"glScissor", (void*)&trt_glScissor},
    {"glDepthRange", (void*)&trt_glDepthRange},
    {"glDepthRangef", (void*)&trt_glDepthRangef},
    {"glClearColor", (void*)&trt_glClearColor},
    {"glClearDepth", (void*)&trt_glClearDepth},
    {"glClearDepthf", (void*)&trt_glClearDepthf},
    {"glClearStencil", (void*)&trt_glClearStencil},
    {"glClear", (void*)&trt_glClear},
    {"glColorMask", (void*)&trt_glColorMask},
    {"glDepthMask", (void*)&trt_glDepthMask},
    {"glDepthFunc", (void*)&trt_glDepthFunc},
    {"glCullFace", (void*)&trt_glCullFace},
    {"glFrontFace", (void*)&trt_glFrontFace},
    {"glPolygonMode", (void*)&trt_glPolygonMode},
    {"glBlendFunc", (void*)&trt_glBlendFunc},
    {"glBlendFuncSeparate", (void*)&trt_glBlendFuncSeparate},
    {"glBlendEquation", (void*)&trt_glBlendEquation},
    {"glBlendEquationSeparate", (void*)&trt_glBlendEquationSeparate},
    {"glBlendColor", (void*)&trt_glBlendColor},
    {"glAlphaFunc", (void*)&trt_glAlphaFunc},
    {"glStencilFunc", (void*)&trt_glStencilFunc},
    {"glStencilMask", (void*)&trt_glStencilMask},
    {"glStencilOp", (void*)&trt_glStencilOp},
    {"glStencilFuncSeparate", (void*)&trt_glStencilFuncSeparate},
    {"glStencilOpSeparate", (void*)&trt_glStencilOpSeparate},
    {"glStencilMaskSeparate", (void*)&trt_glStencilMaskSeparate},
    {"glPixelStorei", (void*)&trt_glPixelStorei},
    {"glPixelStoref", (void*)&trt_glPixelStoref},
    {"glFogf", (void*)&trt_glFogf},
    {"glFogi", (void*)&trt_glFogi},
    {"glFogfv", (void*)&trt_glFogfv},
    {"glLightfv", (void*)&trt_glLightfv},
    {"glLightModelfv", (void*)&trt_glLightModelfv},
    {"glMaterialfv", (void*)&trt_glMaterialfv},
    {"glReadPixels", (void*)&trt_glReadPixels},
    {"glHint", (void*)&trt_glHint},
    {"glFinish", (void*)&trt_glFinish},
    {"glFlush", (void*)&trt_glFlush},
    {"glPolygonOffset", (void*)&trt_glPolygonOffset},
    {"glLineWidth", (void*)&trt_glLineWidth},
    {"glPointSize", (void*)&trt_glPointSize},
    {"glShadeModel", (void*)&trt_glShadeModel},
    {"glReadBuffer", (void*)&trt_glReadBuffer},
    {"glDrawBuffer", (void*)&trt_glDrawBuffer},
    {"glDrawBuffers", (void*)&trt_glDrawBuffers},
    {"glPushAttrib", (void*)&trt_glPushAttrib},
    {"glPopAttrib", (void*)&trt_glPopAttrib},
    {"glPushClientAttrib", (void*)&trt_glPushClientAttrib},
    {"glPopClientAttrib", (void*)&trt_glPopClientAttrib},

    {"glMatrixMode", (void*)&trt_glMatrixMode},
    {"glLoadIdentity", (void*)&trt_glLoadIdentity},
    {"glLoadMatrixf", (void*)&trt_glLoadMatrixf},
    {"glLoadMatrixd", (void*)&trt_glLoadMatrixd},
    {"glMultMatrixf", (void*)&trt_glMultMatrixf},
    {"glMultMatrixd", (void*)&trt_glMultMatrixd},
    {"glPushMatrix", (void*)&trt_glPushMatrix},
    {"glPopMatrix", (void*)&trt_glPopMatrix},
    {"glOrtho", (void*)&trt_glOrtho},
    {"glFrustum", (void*)&trt_glFrustum},
    {"glTranslatef", (void*)&trt_glTranslatef},
    {"glTranslated", (void*)&trt_glTranslated},
    {"glScalef", (void*)&trt_glScalef},
    {"glScaled", (void*)&trt_glScaled},
    {"glRotatef", (void*)&trt_glRotatef},
    {"glRotated", (void*)&trt_glRotated},

    {"glBegin", (void*)&trt_glBegin},
    {"glEnd", (void*)&trt_glEnd},
    {"glVertex2f", (void*)&trt_glVertex2f},
    {"glVertex2i", (void*)&trt_glVertex2i},
    {"glVertex2d", (void*)&trt_glVertex2d},
    {"glVertex2fv", (void*)&trt_glVertex2fv},
    {"glVertex3f", (void*)&trt_glVertex3f},
    {"glVertex3d", (void*)&trt_glVertex3d},
    {"glVertex3fv", (void*)&trt_glVertex3fv},
    {"glVertex4f", (void*)&trt_glVertex4f},
    {"glVertex4fv", (void*)&trt_glVertex4fv},
    {"glNormal3f", (void*)&trt_glNormal3f},
    {"glNormal3fv", (void*)&trt_glNormal3fv},
    {"glColor3f", (void*)&trt_glColor3f},
    {"glColor4f", (void*)&trt_glColor4f},
    {"glColor3fv", (void*)&trt_glColor3fv},
    {"glColor4fv", (void*)&trt_glColor4fv},
    {"glColor3ub", (void*)&trt_glColor3ub},
    {"glColor4ub", (void*)&trt_glColor4ub},
    {"glColor4ubv", (void*)&trt_glColor4ubv},
    {"glTexCoord2f", (void*)&trt_glTexCoord2f},
    {"glTexCoord2d", (void*)&trt_glTexCoord2d},
    {"glTexCoord2fv", (void*)&trt_glTexCoord2fv},
    {"glMultiTexCoord2f", (void*)&trt_glMultiTexCoord2f},
    {"glMultiTexCoord2fARB", (void*)&trt_glMultiTexCoord2f},

    {"glGenLists", (void*)&trt_glGenLists},
    {"glDeleteLists", (void*)&trt_glDeleteLists},
    {"glNewList", (void*)&trt_glNewList},
    {"glEndList", (void*)&trt_glEndList},
    {"glCallList", (void*)&trt_glCallList},
    {"glListBase", (void*)&trt_glListBase},
    {"glCallLists", (void*)&trt_glCallLists},
    {"glIsList", (void*)&trt_glIsList},

    {"glClientActiveTexture", (void*)&trt_glClientActiveTexture},
    {"glClientActiveTextureARB", (void*)&trt_glClientActiveTexture},
    {"glEnableClientState", (void*)&trt_glEnableClientState},
    {"glDisableClientState", (void*)&trt_glDisableClientState},
    {"glVertexPointer", (void*)&trt_glVertexPointer},
    {"glNormalPointer", (void*)&trt_glNormalPointer},
    {"glColorPointer", (void*)&trt_glColorPointer},
    {"glTexCoordPointer", (void*)&trt_glTexCoordPointer},

    {"glDrawArrays", (void*)&trt_glDrawArrays},
    {"glDrawArraysInstanced", (void*)&trt_glDrawArraysInstanced},
    {"glDrawArraysInstancedARB", (void*)&trt_glDrawArraysInstanced},
    {"glDrawElements", (void*)&trt_glDrawElements},
    {"glDrawElementsInstanced", (void*)&trt_glDrawElementsInstanced},
    {"glDrawElementsInstancedARB", (void*)&trt_glDrawElementsInstanced},
    {"glDrawElementsBaseVertex", (void*)&trt_glDrawElementsBaseVertex},
    {"glDrawElementsInstancedBaseVertex", (void*)&trt_glDrawElementsInstancedBaseVertex},
    {"glDrawRangeElements", (void*)&trt_glDrawRangeElements},
    {"glMultiDrawArrays", (void*)&trt_glMultiDrawArrays},
    {"glMultiDrawElements", (void*)&trt_glMultiDrawElements},

    {"glActiveTexture", (void*)&trt_glActiveTexture},
    {"glActiveTextureARB", (void*)&trt_glActiveTexture},
    {"glGenTextures", (void*)&trt_glGenTextures},
    {"glDeleteTextures", (void*)&trt_glDeleteTextures},
    {"glIsTexture", (void*)&trt_glIsTexture},
    {"glBindTexture", (void*)&trt_glBindTexture},
    {"glTexImage2D", (void*)&trt_glTexImage2D},
    {"glTexSubImage2D", (void*)&trt_glTexSubImage2D},
    {"glTexImage1D", (void*)&trt_glTexImage1D},
    {"glTexStorage2D", (void*)&trt_glTexStorage2D},
    {"glCompressedTexImage2D", (void*)&trt_glCompressedTexImage2D},
    {"glCompressedTexImage2DARB", (void*)&trt_glCompressedTexImage2D},
    {"glGenerateMipmap", (void*)&trt_glGenerateMipmap},
    {"glGenerateMipmapEXT", (void*)&trt_glGenerateMipmap},
    {"glTexParameteri", (void*)&trt_glTexParameteri},
    {"glTexParameterf", (void*)&trt_glTexParameterf},
    {"glTexParameteriv", (void*)&trt_glTexParameteriv},
    {"glTexParameterfv", (void*)&trt_glTexParameterfv},
    {"glTexEnvi", (void*)&trt_glTexEnvi},
    {"glTexEnvf", (void*)&trt_glTexEnvf},
    {"glGetTexLevelParameteriv", (void*)&trt_glGetTexLevelParameteriv},
    {"glCopyTexImage2D", (void*)&trt_glCopyTexImage2D},
    {"glCopyTexSubImage2D", (void*)&trt_glCopyTexSubImage2D},
    {"glGetTexImage", (void*)&trt_glGetTexImage},

    {"glGenSamplers", (void*)&trt_glGenSamplers},
    {"glDeleteSamplers", (void*)&trt_glDeleteSamplers},
    {"glBindSampler", (void*)&trt_glBindSampler},
    {"glSamplerParameteri", (void*)&trt_glSamplerParameteri},
    {"glSamplerParameterf", (void*)&trt_glSamplerParameterf},

    {"glGenBuffers", (void*)&trt_glGenBuffers},
    {"glGenBuffersARB", (void*)&trt_glGenBuffers},
    {"glDeleteBuffers", (void*)&trt_glDeleteBuffers},
    {"glDeleteBuffersARB", (void*)&trt_glDeleteBuffers},
    {"glIsBuffer", (void*)&trt_glIsBuffer},
    {"glBindBuffer", (void*)&trt_glBindBuffer},
    {"glBindBufferARB", (void*)&trt_glBindBuffer},
    {"glBindBufferBase", (void*)&trt_glBindBufferBase},
    {"glBindBufferRange", (void*)&trt_glBindBufferRange},
    {"glBufferData", (void*)&trt_glBufferData},
    {"glBufferDataARB", (void*)&trt_glBufferData},
    {"glBufferSubData", (void*)&trt_glBufferSubData},
    {"glBufferSubDataARB", (void*)&trt_glBufferSubData},
    {"glGetBufferSubData", (void*)&trt_glGetBufferSubData},
    {"glMapBuffer", (void*)&trt_glMapBuffer},
    {"glMapBufferARB", (void*)&trt_glMapBuffer},
    {"glMapBufferRange", (void*)&trt_glMapBufferRange},
    {"glFlushMappedBufferRange", (void*)&trt_glFlushMappedBufferRange},
    {"glUnmapBuffer", (void*)&trt_glUnmapBuffer},
    {"glUnmapBufferARB", (void*)&trt_glUnmapBuffer},
    {"glGetBufferParameteriv", (void*)&trt_glGetBufferParameteriv},

    {"glGenVertexArrays", (void*)&trt_glGenVertexArrays},
    {"glDeleteVertexArrays", (void*)&trt_glDeleteVertexArrays},
    {"glBindVertexArray", (void*)&trt_glBindVertexArray},
    {"glIsVertexArray", (void*)&trt_glIsVertexArray},
    {"glEnableVertexAttribArray", (void*)&trt_glEnableVertexAttribArray},
    {"glEnableVertexAttribArrayARB", (void*)&trt_glEnableVertexAttribArray},
    {"glDisableVertexAttribArray", (void*)&trt_glDisableVertexAttribArray},
    {"glDisableVertexAttribArrayARB", (void*)&trt_glDisableVertexAttribArray},
    {"glVertexAttribPointer", (void*)&trt_glVertexAttribPointer},
    {"glVertexAttribPointerARB", (void*)&trt_glVertexAttribPointer},
    {"glVertexAttribIPointer", (void*)&trt_glVertexAttribIPointer},
    {"glVertexAttribDivisor", (void*)&trt_glVertexAttribDivisor},
    {"glVertexAttribDivisorARB", (void*)&trt_glVertexAttribDivisor},
    {"glVertexAttrib1f", (void*)&trt_glVertexAttrib1f},
    {"glVertexAttrib2f", (void*)&trt_glVertexAttrib2f},
    {"glVertexAttrib3f", (void*)&trt_glVertexAttrib3f},
    {"glVertexAttrib4f", (void*)&trt_glVertexAttrib4f},
    {"glVertexAttrib4fv", (void*)&trt_glVertexAttrib4fv},

    {"glCreateShader", (void*)&trt_glCreateShader},
    {"glCreateShaderObjectARB", (void*)&trt_glCreateShader},
    {"glDeleteShader", (void*)&trt_glDeleteShader},
    {"glShaderSource", (void*)&trt_glShaderSource},
    {"glShaderSourceARB", (void*)&trt_glShaderSource},
    {"glCompileShader", (void*)&trt_glCompileShader},
    {"glCompileShaderARB", (void*)&trt_glCompileShader},
    {"glGetShaderiv", (void*)&trt_glGetShaderiv},
    {"glGetShaderInfoLog", (void*)&trt_glGetShaderInfoLog},
    {"glIsShader", (void*)&trt_glIsShader},

    {"glCreateProgram", (void*)&trt_glCreateProgram},
    {"glCreateProgramObjectARB", (void*)&trt_glCreateProgram},
    {"glDeleteProgram", (void*)&trt_glDeleteProgram},
    {"glAttachShader", (void*)&trt_glAttachShader},
    {"glAttachObjectARB", (void*)&trt_glAttachShader},
    {"glDetachShader", (void*)&trt_glDetachShader},
    {"glBindAttribLocation", (void*)&trt_glBindAttribLocation},
    {"glBindAttribLocationARB", (void*)&trt_glBindAttribLocation},
    {"glBindFragDataLocation", (void*)&trt_glBindFragDataLocation},
    {"glLinkProgram", (void*)&trt_glLinkProgram},
    {"glLinkProgramARB", (void*)&trt_glLinkProgram},
    {"glValidateProgram", (void*)&trt_glValidateProgram},
    {"glUseProgram", (void*)&trt_glUseProgram},
    {"glUseProgramObjectARB", (void*)&trt_glUseProgram},
    {"glGetProgramiv", (void*)&trt_glGetProgramiv},
    {"glGetProgramInfoLog", (void*)&trt_glGetProgramInfoLog},
    {"glIsProgram", (void*)&trt_glIsProgram},
    {"glGetAttribLocation", (void*)&trt_glGetAttribLocation},
    {"glGetUniformLocation", (void*)&trt_glGetUniformLocation},
    {"glGetUniformLocationARB", (void*)&trt_glGetUniformLocation},
    {"glGetActiveUniform", (void*)&trt_glGetActiveUniform},
    {"glGetActiveAttrib", (void*)&trt_glGetActiveAttrib},
    {"glGetUniformBlockIndex", (void*)&trt_glGetUniformBlockIndex},
    {"glUniformBlockBinding", (void*)&trt_glUniformBlockBinding},

    {"glUniform1i", (void*)&trt_glUniform1i},
    {"glUniform1iARB", (void*)&trt_glUniform1i},
    {"glUniform2i", (void*)&trt_glUniform2i},
    {"glUniform3i", (void*)&trt_glUniform3i},
    {"glUniform4i", (void*)&trt_glUniform4i},
    {"glUniform1iv", (void*)&trt_glUniform1iv},
    {"glUniform1f", (void*)&trt_glUniform1f},
    {"glUniform1fARB", (void*)&trt_glUniform1f},
    {"glUniform2f", (void*)&trt_glUniform2f},
    {"glUniform3f", (void*)&trt_glUniform3f},
    {"glUniform4f", (void*)&trt_glUniform4f},
    {"glUniform1fv", (void*)&trt_glUniform1fv},
    {"glUniform2fv", (void*)&trt_glUniform2fv},
    {"glUniform3fv", (void*)&trt_glUniform3fv},
    {"glUniform4fv", (void*)&trt_glUniform4fv},
    {"glUniformMatrix4fv", (void*)&trt_glUniformMatrix4fv},
    {"glUniformMatrix4fvARB", (void*)&trt_glUniformMatrix4fv},
    {"glUniformMatrix3fv", (void*)&trt_glUniformMatrix3fv},
    {"glUniformMatrix2fv", (void*)&trt_glUniformMatrix2fv},

    {"glGenFramebuffers", (void*)&trt_glGenFramebuffers},
    {"glGenFramebuffersEXT", (void*)&trt_glGenFramebuffers},
    {"glDeleteFramebuffers", (void*)&trt_glDeleteFramebuffers},
    {"glDeleteFramebuffersEXT", (void*)&trt_glDeleteFramebuffers},
    {"glBindFramebuffer", (void*)&trt_glBindFramebuffer},
    {"glBindFramebufferEXT", (void*)&trt_glBindFramebuffer},
    {"glCheckFramebufferStatus", (void*)&trt_glCheckFramebufferStatus},
    {"glCheckFramebufferStatusEXT", (void*)&trt_glCheckFramebufferStatus},
    {"glFramebufferTexture2D", (void*)&trt_glFramebufferTexture2D},
    {"glFramebufferTexture2DEXT", (void*)&trt_glFramebufferTexture2D},
    {"glFramebufferTexture", (void*)&trt_glFramebufferTexture},
    {"glGenRenderbuffers", (void*)&trt_glGenRenderbuffers},
    {"glGenRenderbuffersEXT", (void*)&trt_glGenRenderbuffers},
    {"glDeleteRenderbuffers", (void*)&trt_glDeleteRenderbuffers},
    {"glBindRenderbuffer", (void*)&trt_glBindRenderbuffer},
    {"glBindRenderbufferEXT", (void*)&trt_glBindRenderbuffer},
    {"glRenderbufferStorage", (void*)&trt_glRenderbufferStorage},
    {"glRenderbufferStorageEXT", (void*)&trt_glRenderbufferStorage},
    {"glRenderbufferStorageMultisample", (void*)&trt_glRenderbufferStorageMultisample},
    {"glFramebufferRenderbuffer", (void*)&trt_glFramebufferRenderbuffer},
    {"glFramebufferRenderbufferEXT", (void*)&trt_glFramebufferRenderbuffer},
    {"glBlitFramebuffer", (void*)&trt_glBlitFramebuffer},

    {"glBindImageTexture", (void*)&trt_glBindImageTexture},
    {"glMemoryBarrier", (void*)&trt_glMemoryBarrier},
    {"glDispatchCompute", (void*)&trt_glDispatchCompute},

    {"glFenceSync", (void*)&trt_glFenceSync},
    {"glDeleteSync", (void*)&trt_glDeleteSync},
    {"glClientWaitSync", (void*)&trt_glClientWaitSync},
    {"glWaitSync", (void*)&trt_glWaitSync},
    {"glGetSynciv", (void*)&trt_glGetSynciv},
    {"glGenQueries", (void*)&trt_glGenQueries},
    {"glDeleteQueries", (void*)&trt_glDeleteQueries},
    {"glBeginQuery", (void*)&trt_glBeginQuery},
    {"glEndQuery", (void*)&trt_glEndQuery},
    {"glGetQueryObjectiv", (void*)&trt_glGetQueryObjectiv},
    {"glGetQueryObjectuiv", (void*)&trt_glGetQueryObjectuiv},
    {"glDebugMessageCallback", (void*)&trt_glDebugMessageCallback},
    {"glDebugMessageControl", (void*)&trt_glDebugMessageControl},

    {"glBlendFuncSeparateEXT", (void*)&trt_glBlendFuncSeparate},
    {"glBlendEquationEXT", (void*)&trt_glBlendEquation},
    {"glBlendEquationSeparateEXT", (void*)&trt_glBlendEquationSeparate},
    {"glBindBufferBaseEXT", (void*)&trt_glBindBufferBase},
    {"glBindBufferRangeEXT", (void*)&trt_glBindBufferRange},
    {"glActiveStencilFaceEXT", (void*)&trt_glHint},
};

const size_t k_all_gl_proc_count = sizeof(k_all_gl_procs) / sizeof(k_all_gl_procs[0]);

}  // namespace

extern "C" {

void trt_gl_set_hooks(const trt_gl_hooks* hooks) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (hooks) {
    g_gl_hooks = *hooks;
    g_gl_hooks_set = true;
  } else {
    memset(&g_gl_hooks, 0, sizeof(g_gl_hooks));
    g_gl_hooks_set = false;
  }
}

int trt_gl_render_hooks_present(void) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  return g_gl_hooks_set && g_gl_hooks.draw && g_gl_hooks.swap_buffers;
}

int trt_wgl_swap_buffers(void* hdc) {
  std::lock_guard<std::mutex> lk(g_gl_mu);
  if (wgl_ctx* c = cur_ctx()) {
    c->hdc = hdc ? hdc : c->hdc;
  }
  if (g_gl_hooks_set && g_gl_hooks.swap_buffers) {
    g_gl_hooks.swap_buffers((u64)(uintptr_t)hdc, g_gl_hooks.user);
    return 1;
  }
  return 1;
}

int trt_bridge_register_opengl32(void) {
  static std::vector<trt_named_fn> s_reg;
  if (s_reg.empty()) {
    s_reg.reserve(k_all_gl_proc_count);
    for (size_t i = 0; i < k_all_gl_proc_count; ++i) {
      s_reg.push_back({k_all_gl_procs[i].name, k_all_gl_procs[i].fn});
    }
  }
  trt_module_register("opengl32.dll", TRT_ROUTE_BUILTIN, s_reg.data(),
                      (int)s_reg.size());
  return 0;
}

}  // extern "C"
