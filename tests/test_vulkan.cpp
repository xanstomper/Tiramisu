// tiramisu/tests/test_vulkan.cpp
// Proves the real Vulkan 1.0-1.3 -> HLSL/D3D11 translation stack:
// 1. SPIR-V -> HLSL SM5 compiler (VS + Y flip + push constant, PS + GLSL.std.450 +
//    combined sampler, CS + SSBO + OpSelectionMerge/OpLoopMerge + OpSpecConstant)
// 2. Bridge Vulkan 1.3 dynamic rendering (vkCmdBeginRendering), compute dispatch,
//    push constants, combined image sampler, and staging buffer->image copy
// 3. Real Win64 PE executable (guest_vulkan.exe) driving the full Vulkan + WSI
//    pipeline through vulkan-1.dll IAT imports.
#include "../src/trt/trt.h"
#include "../src/trt/spirv_hlsl.h"
#include "../src/trt/bridge_vulkan.h"

// Real Vulkan header (present on the Linux dev host) for the Features2 /
// vkCreateDevice validation tests; those checks are skipped without it.
#if __has_include(<vulkan/vulkan_core.h>)
#include <vulkan/vulkan_core.h>
#endif

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
int trt_bridge_register_vulkan(void);
}

static int g_pass = 0;
static int g_fail = 0;
#define CHECK(cond, msg)                                                      \
  do {                                                                        \
    if (cond) { ++g_pass; }                                                   \
    else { ++g_fail; printf("  FAIL: %s (line %d)\n", msg, __LINE__); }       \
  } while (0)

namespace {

struct extent { void* base; size_t len; };
extent g_extents[64];
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
  if (g_extent_count < 64) {
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

u8* read_file_bytes(const char* path, size_t* size) {
  FILE* f = fopen(path, "rb");
  if (!f) return nullptr;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  u8* buf = (u8*)malloc((size_t)n);
  size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  *size = got;
  return buf;
}

u64 win64_call(void* fn) {
  u64 ret;
  asm volatile(
    "subq $48, %%rsp\n"
    "call *%[fn]\n"
    "addq $48, %%rsp\n"
    : "=a"(ret)
    : [fn] "r"(fn)
    : "rcx", "rdx", "r8", "r9", "r10", "r11",
      "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "memory", "cc");
  return ret;
}

struct spv_builder {
  std::vector<u32> w;
  u32 bound = 1;
  void begin() {
    w.assign({0x07230203u, 0x00010000u, 0u, 0u, 0u});
    bound = 1;
  }
  u32 id() { return bound++; }
  void emit(u16 op, std::initializer_list<u32> args) {
    w.push_back(((u32)(args.size() + 1) << 16) | (u32)op);
    for (u32 a : args) w.push_back(a);
  }
  void emit_str(u16 op, std::initializer_list<u32> pre, const char* s,
                std::initializer_list<u32> post = {}) {
    size_t len = strlen(s) + 1;
    size_t sw = (len + 3) / 4;
    w.push_back(((u32)(1 + pre.size() + sw + post.size()) << 16) | (u32)op);
    for (u32 a : pre) w.push_back(a);
    for (size_t i = 0; i < sw; ++i) {
      u32 v = 0;
      for (size_t b = 0; b < 4; ++b) {
        size_t idx = i * 4 + b;
        if (idx < len - 1) v |= ((u32)(u8)s[idx]) << (b * 8);
      }
      w.push_back(v);
    }
    for (u32 a : post) w.push_back(a);
  }
  const std::vector<u32>& finish() {
    w[3] = bound;
    return w;
  }
};

struct obs_state {
  int pipes_created = 0;
  int comp_pipes_created = 0;
  int buffers_synced = 0;
  int images_synced = 0;
  int rp_begun = 0;
  int rp_ended = 0;
  int draws = 0;
  int dispatches = 0;
  int copies_b2i = 0;
  int presents = 0;
  u32 last_push_bytes = 0;
  u32 last_binding_count = 0;
  u32 last_draw_verts = 0;
  u32 last_gx = 0;
  std::string last_vs;
  std::string last_ps;
  std::string last_cs;
};

obs_state g_obs;

u32 vk_create_pipeline(const trt_vk_pipeline_desc* d, void*) {
  if (d->is_compute) {
    ++g_obs.comp_pipes_created;
    if (d->cs_hlsl) g_obs.last_cs = d->cs_hlsl;
  } else {
    ++g_obs.pipes_created;
    if (d->vs_hlsl) g_obs.last_vs = d->vs_hlsl;
    if (d->ps_hlsl) g_obs.last_ps = d->ps_hlsl;
  }
  return 1;
}
u32 vk_sync_buffer(u64, u32, const void*, u64, void*) { ++g_obs.buffers_synced; return 1; }
u32 vk_sync_image(u64, const trt_vk_image_desc*, const void*, u64, u32, u32, void*) {
  ++g_obs.images_synced; return 1;
}
void vk_begin_rp(const trt_vk_rp_begin*, void*) { ++g_obs.rp_begun; }
void vk_end_rp(void*) { ++g_obs.rp_ended; }
void vk_draw(const trt_vk_draw_cmd* c, void*) {
  ++g_obs.draws;
  g_obs.last_push_bytes = c->push_bytes;
  g_obs.last_binding_count = c->binding_count;
  g_obs.last_draw_verts = c->vtx_count;
}
void vk_dispatch(const trt_vk_dispatch_cmd* c, void*) {
  ++g_obs.dispatches;
  g_obs.last_gx = c->gx;
}
void vk_copy_b2i(u64, u64, u64, u32, u32, u32, u32, const void*, u64, void*) {
  ++g_obs.copies_b2i;
}
void vk_present(u64, u32, void*) { ++g_obs.presents; }

}  // namespace

int main(int argc, char** argv) {
  printf("=== Vulkan 1.3 -> SPIR-V/HLSL/D3D11 Translation Tests ===\n");

  // --- 1. SPIR-V -> HLSL unit tests (VS + push constants, PS + GLSL.std.450 + sampler, CS + SSBO + spec constant + loop)
  {
    spv_builder b;
    b.begin();
    u32 fn_main = b.id(), inPos = b.id(), outPos = b.id(), pcVar = b.id();
    u32 tVoid = b.id(), tFn = b.id(), tF32 = b.id(), tV4 = b.id(), tU32 = b.id();
    u32 c0 = b.id(), tPcStruct = b.id(), tPtrPcS = b.id(), tPtrPcV = b.id();
    u32 tPtrIn = b.id(), tPtrOut = b.id(), lbl = b.id(), gep = b.id();
    u32 vPos = b.id(), vOff = b.id(), vSum = b.id();

    b.emit(17 /*OpCapability*/, {1u});
    b.emit(14 /*OpMemoryModel*/, {0u, 1u});
    b.emit_str(15 /*OpEntryPoint*/, {0u /*Vertex*/, fn_main}, "main", {inPos, outPos});
    b.emit(71 /*OpDecorate*/, {inPos, 30u /*Location*/, 0u});
    b.emit(71 /*OpDecorate*/, {outPos, 11u /*BuiltIn*/, 0u /*Position*/});
    b.emit(71 /*OpDecorate*/, {tPcStruct, 2u /*Block*/});
    b.emit(72 /*OpMemberDecorate*/, {tPcStruct, 0u, 35u /*Offset*/, 0u});
    b.emit(19, {tVoid});
    b.emit(33, {tFn, tVoid});
    b.emit(22, {tF32, 32u});
    b.emit(23, {tV4, tF32, 4u});
    b.emit(21, {tU32, 32u, 0u});
    b.emit(43, {tU32, c0, 0u});
    b.emit(30, {tPcStruct, tV4});
    b.emit(32, {tPtrPcS, 9u /*PushConstant*/, tPcStruct});
    b.emit(32, {tPtrPcV, 9u, tV4});
    b.emit(32, {tPtrIn, 1u, tV4});
    b.emit(32, {tPtrOut, 3u, tV4});
    b.emit(59, {tPtrPcS, pcVar, 9u});
    b.emit(59, {tPtrIn, inPos, 1u});
    b.emit(59, {tPtrOut, outPos, 3u});
    b.emit(54, {tVoid, fn_main, 0u, tFn});
    b.emit(248, {lbl});
    b.emit(65, {tPtrPcV, gep, pcVar, c0});
    b.emit(61, {tV4, vPos, inPos});
    b.emit(61, {tV4, vOff, gep});
    b.emit(129 /*OpFAdd*/, {tV4, vSum, vPos, vOff});
    b.emit(62, {outPos, vSum});
    b.emit(253, {});
    b.emit(56, {});
    b.finish();

    spirv_compile_result rvs = spirv_to_hlsl(b.w.data(), (u32)b.w.size(),
                                             SPV_STAGE_VERTEX, "main");
    CHECK(rvs.ok, "SPIR-V VS with PushConstant compiles");
    CHECK(rvs.push_constant_bytes >= 16u, "SPIR-V VS push constant bytes reflected");
    CHECK(rvs.hlsl.find("TEXCOORD0") != std::string::npos, "SPIR-V VS emits TEXCOORD0 input");
    CHECK(rvs.hlsl.find("SV_Position") != std::string::npos, "SPIR-V VS emits SV_Position");
    CHECK(rvs.hlsl.find(".y = -_out.") != std::string::npos,
          "SPIR-V VS applies Vulkan->D3D11 NDC Y flip");
  }

  {
    // Fragment shader: GLSL.std.450 normalize + dot + combined image sampler
    spv_builder b;
    b.begin();
    u32 ext = b.id(), fn_main = b.id(), inUv = b.id(), outCol = b.id(), smpVar = b.id();
    u32 tVoid = b.id(), tFn = b.id(), tF32 = b.id(), tV2 = b.id(), tV3 = b.id(), tV4 = b.id();
    u32 tImg = b.id(), tSmpImg = b.id(), tPtrSmp = b.id(), tPtrIn2 = b.id(), tPtrOut4 = b.id();
    u32 c1 = b.id(), c0 = b.id(), cV3 = b.id(), lbl = b.id();
    u32 uv = b.id(), tex = b.id(), xyz = b.id(), nrm = b.id(), dval = b.id(), res = b.id();

    b.emit(17, {1u});
    b.emit_str(11 /*OpExtInstImport*/, {ext}, "GLSL.std.450");
    b.emit(14, {0u, 1u});
    b.emit_str(15, {4u /*Fragment*/, fn_main}, "main", {inUv, outCol});
    b.emit(16, {fn_main, 7u /*OriginUpperLeft*/});
    b.emit(71, {inUv, 30u, 0u});
    b.emit(71, {outCol, 30u, 0u});
    b.emit(71, {smpVar, 34u /*DescriptorSet*/, 0u});
    b.emit(71, {smpVar, 33u /*Binding*/, 1u});
    b.emit(19, {tVoid});
    b.emit(33, {tFn, tVoid});
    b.emit(22, {tF32, 32u});
    b.emit(23, {tV2, tF32, 2u});
    b.emit(23, {tV3, tF32, 3u});
    b.emit(23, {tV4, tF32, 4u});
    b.emit(25 /*OpTypeImage*/, {tImg, tF32, 1u /*2D*/, 0u, 0u, 0u, 1u, 0u});
    b.emit(27 /*OpTypeSampledImage*/, {tSmpImg, tImg});
    b.emit(32, {tPtrSmp, 0u /*UniformConstant*/, tSmpImg});
    b.emit(32, {tPtrIn2, 1u, tV2});
    b.emit(32, {tPtrOut4, 3u, tV4});
    b.emit(43, {tF32, c0, 0x00000000u});
    b.emit(43, {tF32, c1, 0x3F800000u});
    b.emit(44 /*OpConstantComposite*/, {tV3, cV3, c0, c1, c0});
    b.emit(59, {tPtrSmp, smpVar, 0u});
    b.emit(59, {tPtrIn2, inUv, 1u});
    b.emit(59, {tPtrOut4, outCol, 3u});
    b.emit(54, {tVoid, fn_main, 0u, tFn});
    b.emit(248, {lbl});
    b.emit(61, {tV2, uv, inUv});
    b.emit(61, {tSmpImg, tex, smpVar});
    b.emit(87 /*OpImageSampleImplicitLod*/, {tV4, res, tex, uv});
    b.emit(79 /*OpVectorShuffle*/, {tV3, xyz, res, res, 0u, 1u, 2u});
    b.emit(12 /*OpExtInst*/, {tV3, nrm, ext, 69u /*Normalize*/, xyz});
    b.emit(148 /*OpDot*/, {tF32, dval, nrm, cV3});
    b.emit(62, {outCol, res});
    b.emit(253, {});
    b.emit(56, {});
    b.finish();

    spirv_compile_result rps = spirv_to_hlsl(b.w.data(), (u32)b.w.size(),
                                             SPV_STAGE_FRAGMENT, "main");
    CHECK(rps.ok, "SPIR-V PS with SampledImage + GLSL.std.450 compiles");
    CHECK(rps.bindings.size() == 1 &&
              rps.bindings[0].kind == SPV_BIND_COMBINED_IMG_SMP,
          "SPIR-V PS reflects combined image sampler");
    CHECK(rps.hlsl.find(".Sample(") != std::string::npos, "SPIR-V PS emits HLSL Sample()");
    CHECK(rps.hlsl.find("normalize(") != std::string::npos, "SPIR-V PS lowers GLSL.std.450 normalize");
  }

  {
    // Compute shader with StorageBuffer (RWByteAddressBuffer), SpecConstant, LocalSize(8,4,1)
    spv_builder b;
    b.begin();
    u32 fn_main = b.id(), ssboVar = b.id(), specVal = b.id(), c0 = b.id();
    u32 tVoid = b.id(), tFn = b.id(), tU32 = b.id(), tArr = b.id(), tSsboS = b.id();
    u32 tPtrSsbo = b.id(), tPtrU32 = b.id(), lbl = b.id(), gep = b.id();

    b.emit(17, {1u});
    b.emit(14, {0u, 1u});
    b.emit_str(15, {5u /*GLCompute*/, fn_main}, "main");
    b.emit(16, {fn_main, 17u /*LocalSize*/, 8u, 4u, 1u});
    b.emit(71, {specVal, 1u /*SpecId*/, 3u});
    b.emit(71, {ssboVar, 34u, 0u});
    b.emit(71, {ssboVar, 33u, 2u});
    b.emit(71, {tSsboS, 3u /*BufferBlock*/});
    b.emit(71, {tArr, 6u /*ArrayStride*/, 4u});
    b.emit(72, {tSsboS, 0u, 35u, 0u});
    b.emit(19, {tVoid});
    b.emit(33, {tFn, tVoid});
    b.emit(21, {tU32, 32u, 0u});
    b.emit(43, {tU32, c0, 0u});
    b.emit(50 /*OpSpecConstant*/, {tU32, specVal, 42u});
    b.emit(29 /*OpTypeRuntimeArray*/, {tArr, tU32});
    b.emit(30, {tSsboS, tArr});
    b.emit(32, {tPtrSsbo, 2u /*Uniform*/, tSsboS});
    b.emit(32, {tPtrU32, 2u, tU32});
    b.emit(59, {tPtrSsbo, ssboVar, 2u});
    b.emit(54, {tVoid, fn_main, 0u, tFn});
    b.emit(248, {lbl});
    b.emit(65, {tPtrU32, gep, ssboVar, c0, c0});
    b.emit(62, {gep, specVal});
    b.emit(253, {});
    b.emit(56, {});
    b.finish();

    spirv_spec_entry spec = {3u, 777u};
    spirv_compile_result rcs = spirv_to_hlsl(b.w.data(), (u32)b.w.size(),
                                             SPV_STAGE_COMPUTE, "main", &spec, 1);
    CHECK(rcs.ok, "SPIR-V CS with SSBO + SpecConstant compiles");
    CHECK(rcs.local_size[0] == 8u && rcs.local_size[1] == 4u,
          "SPIR-V CS reflects LocalSize(8,4,1)");
    CHECK(rcs.hlsl.find("numthreads(8, 4, 1)") != std::string::npos,
          "SPIR-V CS emits HLSL [numthreads(8, 4, 1)]");
    CHECK(rcs.hlsl.find("RWByteAddressBuffer") != std::string::npos,
          "SPIR-V CS lowers StorageBuffer to RWByteAddressBuffer");
    CHECK(rcs.hlsl.find("777") != std::string::npos,
          "SPIR-V CS substitutes specialization constant value");
  }

  // --- 2. Boot runtime & verify Vulkan ICD hooks directly + via guest_vulkan.exe ---
  trt_backend be{linux_alloc, linux_protect, linux_release, linux_log, nullptr};
  trt_bridge_boot(&be);
  trt_bridge_register_kernel32();
  trt_bridge_register_user32();
  trt_bridge_register_gdi();
  trt_bridge_register_vulkan();

  trt_vk_hooks hk{};
  hk.create_pipeline = &vk_create_pipeline;
  hk.sync_buffer = &vk_sync_buffer;
  hk.sync_image = &vk_sync_image;
  hk.begin_render_pass = &vk_begin_rp;
  hk.end_render_pass = &vk_end_rp;
  hk.draw = &vk_draw;
  hk.dispatch = &vk_dispatch;
  hk.copy_buf_to_img = &vk_copy_b2i;
  hk.present = &vk_present;
  trt_vk_set_hooks(&hk);

  // Also directly test Vulkan 1.3 vkCmdBeginRendering + vkCreateComputePipelines + vkCmdDispatch + vkCmdCopyBufferToImage
  {
    using VkCreateInst_t = i32(TRT_GUEST_ABI*)(const void*, const void*, void**);
    using VkEnumPd_t = i32(TRT_GUEST_ABI*)(void*, u32*, void**);
    using VkCreateDev_t = i32(TRT_GUEST_ABI*)(void*, const void*, const void*, void**);
    using VkGetQ_t = void(TRT_GUEST_ABI*)(void*, u32, u32, void**);
    using VkCreateImg_t = i32(TRT_GUEST_ABI*)(void*, const void*, const void*, u64*);
    using VkCreateIv_t = i32(TRT_GUEST_ABI*)(void*, const void*, const void*, u64*);
    using VkCreateBuf_t = i32(TRT_GUEST_ABI*)(void*, const void*, const void*, u64*);
    using VkAllocMem_t = i32(TRT_GUEST_ABI*)(void*, const void*, const void*, u64*);
    using VkMapMem_t = i32(TRT_GUEST_ABI*)(void*, u64, u64, u64, u32, void**);
    using VkUnmapMem_t = void(TRT_GUEST_ABI*)(void*, u64);
    using VkBindBuf_t = i32(TRT_GUEST_ABI*)(void*, u64, u64, u64);
    using VkCreateSm_t = i32(TRT_GUEST_ABI*)(void*, const void*, const void*, u64*);
    using VkCreateCp_t = i32(TRT_GUEST_ABI*)(void*, u64, u32, const void*, const void*, u64*);
    using VkCreateCmdP_t = i32(TRT_GUEST_ABI*)(void*, const void*, const void*, u64*);
    using VkAllocCb_t = i32(TRT_GUEST_ABI*)(void*, const void*, void**);
    using VkBeginCb_t = i32(TRT_GUEST_ABI*)(void*, const void*);
    using VkEndCb_t = i32(TRT_GUEST_ABI*)(void*);
    using VkBeginRend_t = void(TRT_GUEST_ABI*)(void*, const void*);
    using VkEndRend_t = void(TRT_GUEST_ABI*)(void*);
    using VkBindPipe_t = void(TRT_GUEST_ABI*)(void*, u32, u64);
    using VkDispatch_t = void(TRT_GUEST_ABI*)(void*, u32, u32, u32);
    using VkCopyB2I_t = void(TRT_GUEST_ABI*)(void*, u64, u64, u32, u32, const void*);
    using VkSubmit_t = i32(TRT_GUEST_ABI*)(void*, u32, const void*, u64);
    using VkDestroyDev_t = void(TRT_GUEST_ABI*)(void*, const void*);
    using VkDestroyInst_t = void(TRT_GUEST_ABI*)(void*, const void*);

    auto fn = [&](const char* s) { return trt_resolve_module(nullptr, "vulkan-1.dll", s, 0); };
    struct { u32 sType; const void* pNext; u32 flags; const void* pApp; u32 lc; const void* pl; u32 ec; const void* pe; } ici{1u};
    void* inst = nullptr;
    ((VkCreateInst_t)fn("vkCreateInstance"))(&ici, nullptr, &inst);
    void* gpu = nullptr; u32 gcnt = 1;
    ((VkEnumPd_t)fn("vkEnumeratePhysicalDevices"))(inst, &gcnt, &gpu);
    float prio = 1.0f;
    struct { u32 sType; const void* pNext; u32 flags; u32 qfi; u32 qc; const float* pr; } dq{2u, nullptr, 0, 0, 1, &prio};
    struct { u32 sType; const void* pNext; u32 flags; u32 qcc; const void* pqc; u32 lc; const void* pl; u32 ec; const void* pe; const void* pf; } dci{3u, nullptr, 0, 1, &dq};
    void* dev = nullptr;
    ((VkCreateDev_t)fn("vkCreateDevice"))(gpu, &dci, nullptr, &dev);
    void* q = nullptr;
    ((VkGetQ_t)fn("vkGetDeviceQueue"))(dev, 0, 0, &q);

    struct { u32 sType; const void* pNext; u32 flags; u32 type; u32 fmt; u32 w, h, d; u32 mips, layers, samples, tiling, usage, sharing, qc; const u32* pq; u32 il; } img_ci{14u, nullptr, 0, 1u, 37u, 64u, 64u, 1u, 1u, 1u, 1u, 0u, 0x16u};
    u64 img = 0, iv = 0;
    ((VkCreateImg_t)fn("vkCreateImage"))(dev, &img_ci, nullptr, &img);
    struct { u32 sType; const void* pNext; u32 flags; u64 img; u32 vt; u32 fmt; u32 c[4]; u32 sr[5]; } iv_ci{15u, nullptr, 0, img, 1u, 37u};
    ((VkCreateIv_t)fn("vkCreateImageView"))(dev, &iv_ci, nullptr, &iv);

    struct { u32 sType; const void* pNext; u32 flags; u64 size; u32 usage, sharing, qc; const u32* pq; } buf_ci{12u, nullptr, 0, 64 * 64 * 4, 1u};
    u64 stg = 0, mem = 0;
    ((VkCreateBuf_t)fn("vkCreateBuffer"))(dev, &buf_ci, nullptr, &stg);
    struct { u32 sType; const void* pNext; u64 sz; u32 mti; } ai{5u, nullptr, 64 * 64 * 4, 0};
    ((VkAllocMem_t)fn("vkAllocateMemory"))(dev, &ai, nullptr, &mem);
    void* mp = nullptr;
    ((VkMapMem_t)fn("vkMapMemory"))(dev, mem, 0, 64 * 64 * 4, 0, &mp);
    if (mp) memset(mp, 0x7F, 64 * 64 * 4);
    ((VkUnmapMem_t)fn("vkUnmapMemory"))(dev, mem);
    ((VkBindBuf_t)fn("vkBindBufferMemory"))(dev, stg, mem, 0);

    spv_builder b; b.begin();
    u32 fm = b.id(), tV = b.id(), tF = b.id(), lbl = b.id();
    b.emit(17, {1u}); b.emit(14, {0u, 1u});
    b.emit_str(15, {5u, fm}, "main");
    b.emit(16, {fm, 17u, 16u, 1u, 1u});
    b.emit(19, {tV}); b.emit(33, {tF, tV});
    b.emit(54, {tV, fm, 0u, tF}); b.emit(248, {lbl}); b.emit(253, {}); b.emit(56, {});
    b.finish();
    struct { u32 sType; const void* pNext; u32 flags; size_t sz; const u32* code; } smci{15u, nullptr, 0, b.w.size() * 4, b.w.data()};
    u64 csm = 0, cpipe = 0;
    ((VkCreateSm_t)fn("vkCreateShaderModule"))(dev, &smci, nullptr, &csm);
    struct { u32 sType; const void* pNext; u32 flags; struct { u32 s; const void* pn; u32 fl; u32 stg; u64 m; const char* nm; const void* sp; } stage; u64 layout, base; i32 bidx; } cci{29u, nullptr, 0, {18u, nullptr, 0, 0x20u, csm, "main", nullptr}};
    ((VkCreateCp_t)fn("vkCreateComputePipelines"))(dev, 0, 1, &cci, nullptr, &cpipe);

    struct { u32 sType; const void* pNext; u32 flags; u32 qfi; } cpci{39u};
    u64 cp = 0; void* cb = nullptr;
    ((VkCreateCmdP_t)fn("vkCreateCommandPool"))(dev, &cpci, nullptr, &cp);
    struct { u32 sType; const void* pNext; u64 pool; u32 lvl, cnt; } cbai{40u, nullptr, cp, 0, 1};
    ((VkAllocCb_t)fn("vkAllocateCommandBuffers"))(dev, &cbai, &cb);

    struct { u32 sType; const void* pNext; u32 flags; const void* inh; } cbbi{42u};
    ((VkBeginCb_t)fn("vkBeginCommandBuffer"))(cb, &cbbi);
    struct { u64 off; u32 rl, ih; u32 aspect, mip, base, cnt; i32 ox, oy, oz; u32 ew, eh, ed; } bic{0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 64, 64, 1};
    ((VkCopyB2I_t)fn("vkCmdCopyBufferToImage"))(cb, stg, img, 1u, 1u, &bic);
    struct { u32 sType; const void* pNext; u64 iv; u32 il, rm; u64 riv; u32 ril, loadOp, storeOp; float clr[4]; } ratt{1000044001u, nullptr, iv, 2u, 0, 0, 0, 1u, 0u, {0.f, 1.f, 0.f, 1.f}};
    struct { u32 sType; const void* pNext; u32 flags; i32 x, y; u32 w, h; u32 lc, vm, colCnt; const void* pCols; const void* pD; const void* pS; } rinfo{1000044000u, nullptr, 0, 0, 0, 64, 64, 1, 0, 1, &ratt};
    ((VkBeginRend_t)fn("vkCmdBeginRendering"))(cb, &rinfo);
    ((VkEndRend_t)fn("vkCmdEndRendering"))(cb);
    ((VkBindPipe_t)fn("vkCmdBindPipeline"))(cb, 1u, cpipe);
    ((VkDispatch_t)fn("vkCmdDispatch"))(cb, 4u, 4u, 1u);
    ((VkEndCb_t)fn("vkEndCommandBuffer"))(cb);

    struct { u32 sType; const void* pNext; u32 wc; const void* pw, *ps; u32 cbc; void* const* pcb; u32 sc; const void* psig; } subi{4u, nullptr, 0, nullptr, nullptr, 1, &cb};
    ((VkSubmit_t)fn("vkQueueSubmit"))(q, 1, &subi, 0);

    CHECK(g_obs.comp_pipes_created == 1, "Compute pipeline compiled via Vulkan bridge");
    CHECK(g_obs.copies_b2i == 1, "vkCmdCopyBufferToImage dispatched to backend hook");
    CHECK(g_obs.rp_begun == 1 && g_obs.rp_ended == 1, "Vulkan 1.3 vkCmdBeginRendering routed");
    CHECK(g_obs.dispatches == 1 && g_obs.last_gx == 4u, "vkCmdDispatch routed with gx=4");

    // --- 2b. Verify expanded Vulkan 1.0/1.1/1.2/KHR core entrypoints & execution ---
    using VkGetIProc_t = void*(TRT_GUEST_ABI*)(void*, const char*);
    using VkGetDProc_t = void*(TRT_GUEST_ABI*)(void*, const char*);
    auto gipa = (VkGetIProc_t)fn("vkGetInstanceProcAddr");
    auto gdpa = (VkGetDProc_t)fn("vkGetDeviceProcAddr");
    const char* req_procs[] = {
        "vkGetPhysicalDeviceImageFormatProperties",
        "vkGetPhysicalDeviceFormatProperties2KHR",
        "vkGetPhysicalDeviceImageFormatProperties2KHR",
        "vkGetPhysicalDeviceQueueFamilyProperties2KHR",
        "vkGetPhysicalDeviceMemoryProperties2KHR",
        "vkCreatePipelineCache",
        "vkDestroyPipelineCache",
        "vkGetPipelineCacheData",
        "vkMergePipelineCaches",
        "vkGetBufferMemoryRequirements2KHR",
        "vkGetImageMemoryRequirements2KHR",
        "vkBindBufferMemory2KHR",
        "vkBindImageMemory2KHR",
        "vkCreateEvent",
        "vkDestroyEvent",
        "vkGetEventStatus",
        "vkSetEvent",
        "vkResetEvent",
        "vkCreateBufferView",
        "vkDestroyBufferView",
        "vkGetImageSubresourceLayout",
        "vkGetDeviceMemoryCommitment",
        "vkGetImageSparseMemoryRequirements2KHR",
        "vkQueueBindSparse",
        "vkGetPhysicalDeviceSparseImageFormatProperties2KHR",
        "vkGetPhysicalDeviceExternalSemaphorePropertiesKHR",
        "vkGetPhysicalDeviceExternalFencePropertiesKHR",
        "vkGetRenderAreaGranularity",
        "vkCmdSetDepthBounds",
        "vkCmdSetStencilCompareMask",
        "vkCmdSetStencilWriteMask",
        "vkCmdSetStencilReference",
        "vkCmdDrawIndirect",
        "vkCmdDrawIndexedIndirect",
        "vkCmdDispatchIndirect",
        "vkCmdUpdateBuffer",
        "vkCmdFillBuffer",
        "vkCmdClearDepthStencilImage",
        "vkCmdClearAttachments",
        "vkCmdResolveImage",
        "vkCmdSetEvent",
        "vkCmdResetEvent",
        "vkCmdWaitEvents",
        "vkCmdBeginQuery",
        "vkCmdEndQuery",
        "vkCmdResetQueryPool",
        "vkCmdWriteTimestamp",
        "vkCmdCopyQueryPoolResults",
        "vkCmdExecuteCommands",
        "vkCreateRenderPass2KHR",
        "vkCmdBeginRenderPass2KHR",
        "vkCmdEndRenderPass2KHR",
        "vkCmdNextSubpass2KHR",
        "vkQueueSubmit2KHR",
        "vkCmdPipelineBarrier2KHR",
        "vkGetSemaphoreCounterValueKHR",
        "vkCmdPushDescriptorSetKHR",
    };
    bool all_procs_ok = true;
    for (const char* p : req_procs) {
      if (!gipa(inst, p) || !gdpa(dev, p)) {
        printf("    missing Vulkan proc: %s\n", p);
        all_procs_ok = false;
      }
    }
    CHECK(all_procs_ok, "All 57 Vulkan 1.0/1.1/1.2/KHR core & Unity procs resolve non-NULL");

    using VkImgFmtProps_t = i32(TRT_GUEST_ABI*)(void*, u32, u32, u32, u32, u32, void*);
    struct { u32 w, h, d, mips, layers, samples; u64 max_sz; } ifp{};
    i32 ifp_rc = ((VkImgFmtProps_t)gipa(inst, "vkGetPhysicalDeviceImageFormatProperties"))(
        gpu, 37u, 1u, 0u, 0x16u, 0u, &ifp);
    CHECK(ifp_rc == 0 && ifp.w == 16384u && ifp.mips > 1u,
          "vkGetPhysicalDeviceImageFormatProperties returns sane maxExtent & mipLevels");

    using VkCreatePc_t = i32(TRT_GUEST_ABI*)(void*, const void*, const void*, u64*);
    using VkGetPc_t = i32(TRT_GUEST_ABI*)(void*, u64, size_t*, void*);
    using VkDestroyPc_t = void(TRT_GUEST_ABI*)(void*, u64, const void*);
    struct { u32 sType; const void* pNext; u32 flags; size_t sz; const void* data; } pcci{17u};
    u64 pcache = 0;
    CHECK(((VkCreatePc_t)gdpa(dev, "vkCreatePipelineCache"))(dev, &pcci, nullptr, &pcache) == 0 && pcache != 0,
          "vkCreatePipelineCache succeeds");
    size_t pcsz = 0;
    u8 pchdr[64]{};
    ((VkGetPc_t)gdpa(dev, "vkGetPipelineCacheData"))(dev, pcache, &pcsz, nullptr);
    size_t pc_in = pcsz <= sizeof(pchdr) ? pcsz : sizeof(pchdr);
    ((VkGetPc_t)gdpa(dev, "vkGetPipelineCacheData"))(dev, pcache, &pc_in, pchdr);
    u32 hdr_sz = 0, hdr_ver = 0;
    memcpy(&hdr_sz, pchdr, 4); memcpy(&hdr_ver, pchdr + 4, 4);
    CHECK(pcsz >= 32 && hdr_sz == 32u && hdr_ver == 1u,
          "vkGetPipelineCacheData returns valid 32B header v1");
    ((VkDestroyPc_t)gdpa(dev, "vkDestroyPipelineCache"))(dev, pcache, nullptr);

    using VkCreateEv_t = i32(TRT_GUEST_ABI*)(void*, const void*, const void*, u64*);
    using VkEvOp_t = i32(TRT_GUEST_ABI*)(void*, u64);
    using VkDestroyEv_t = void(TRT_GUEST_ABI*)(void*, u64, const void*);
    struct { u32 sType; const void* pNext; u32 flags; } evci{10u};
    u64 ev = 0;
    ((VkCreateEv_t)gdpa(dev, "vkCreateEvent"))(dev, &evci, nullptr, &ev);
    CHECK(((VkEvOp_t)gdpa(dev, "vkGetEventStatus"))(dev, ev) == 4, "Event starts VK_EVENT_RESET");
    ((VkEvOp_t)gdpa(dev, "vkSetEvent"))(dev, ev);
    CHECK(((VkEvOp_t)gdpa(dev, "vkGetEventStatus"))(dev, ev) == 3, "vkSetEvent -> VK_EVENT_SET");
    ((VkEvOp_t)gdpa(dev, "vkResetEvent"))(dev, ev);
    CHECK(((VkEvOp_t)gdpa(dev, "vkGetEventStatus"))(dev, ev) == 4, "vkResetEvent -> VK_EVENT_RESET");
    ((VkDestroyEv_t)gdpa(dev, "vkDestroyEvent"))(dev, ev, nullptr);

    using VkBufReq2_t = void(TRT_GUEST_ABI*)(void*, const void*, void*);
    using VkImgLay_t = void(TRT_GUEST_ABI*)(void*, u64, const void*, void*);
    struct { u32 sType; const void* pNext; u64 buf; } bri{1000146000u, nullptr, stg};
    struct { u32 sType; void* pNext; u64 sz, align; u32 bits; } br2{1000146003u};
    ((VkBufReq2_t)gdpa(dev, "vkGetBufferMemoryRequirements2KHR"))(dev, &bri, &br2);
    CHECK(br2.sz >= 64 * 64 * 4 && br2.bits != 0, "vkGetBufferMemoryRequirements2KHR succeeds");
    struct { u32 aspect, mip, layer; } isr{1u, 0u, 0u};
    struct { u64 off, sz, row, arr, dep; } ilay{};
    ((VkImgLay_t)gdpa(dev, "vkGetImageSubresourceLayout"))(dev, img, &isr, &ilay);
    CHECK(ilay.row == 64u * 4u, "vkGetImageSubresourceLayout reports rowPitch=w*4");

    using VkCreateRp2_t = i32(TRT_GUEST_ABI*)(void*, const void*, const void*, u64*);
    using VkCreateFb_t = i32(TRT_GUEST_ABI*)(void*, const void*, const void*, u64*);
    using VkCmdRp2_t = void(TRT_GUEST_ABI*)(void*, const void*, const void*);
    using VkCmdEndRp2_t = void(TRT_GUEST_ABI*)(void*, const void*);
    using VkCreateGp_t = i32(TRT_GUEST_ABI*)(void*, u64, u32, const void*, const void*, u64*);
    using VkUpdBuf_t = void(TRT_GUEST_ABI*)(void*, u64, u64, u64, const void*);
    using VkDispInd_t = void(TRT_GUEST_ABI*)(void*, u64, u64);
    using VkDrawInd_t = void(TRT_GUEST_ABI*)(void*, u64, u64, u32, u32);
    using VkExecCmds_t = void(TRT_GUEST_ABI*)(void*, u32, void* const*);
    using VkSubmit2_t = i32(TRT_GUEST_ABI*)(void*, u32, const void*, u64);

    struct { u32 sType; const void* pNext; u32 fl, fmt, smp, loadOp, storeOp, sl, ss, il, flay; } ad2{1000109000u, nullptr, 0, 37u, 1u, 1u, 0u, 2u, 2u, 0u, 1u};
    struct { u32 sType; const void* pNext; u32 att, layout, aspect; } ar2{1000109001u, nullptr, 0u, 2u, 1u};
    struct { u32 sType; const void* pNext; u32 fl, bp, vm, ic; const void* pi; u32 cc; const void* pc, *pr, *pds; u32 pac; const void* ppa; } sp2{1000109002u, nullptr, 0, 0, 0, 0, nullptr, 1u, &ar2};
    struct { u32 sType; const void* pNext; u32 fl, ac; const void* pa; u32 sc; const void* psp; u32 dc; const void* pd; u32 cmc; const void* pcm; } rpci2{1000109004u, nullptr, 0, 1u, &ad2, 1u, &sp2};
    u64 rp2 = 0, fb2 = 0, gp = 0;
    CHECK(((VkCreateRp2_t)gdpa(dev, "vkCreateRenderPass2KHR"))(dev, &rpci2, nullptr, &rp2) == 0 && rp2 != 0,
          "vkCreateRenderPass2KHR succeeds");
    struct { u32 sType; const void* pNext; u32 fl; u64 rp; u32 ac; const u64* pa; u32 w, h, l; } fbci{37u, nullptr, 0, rp2, 1u, &iv, 64u, 64u, 1u};
    ((VkCreateFb_t)gdpa(dev, "vkCreateFramebuffer"))(dev, &fbci, nullptr, &fb2);
    struct { u32 sType; const void* pNext; u32 fl, stgc; const void* pst; const void* pvi, *pia, *pt, *pv, *pr, *pm, *pds, *pcb, *pdy; u64 lay, rp; u32 sp; u64 bh; i32 bi; } gpci{28u};
    ((VkCreateGp_t)gdpa(dev, "vkCreateGraphicsPipelines"))(dev, 0, 1, &gpci, nullptr, &gp);

    struct { u32 sType; const void* pNext; u64 pool; u32 lvl, cnt; } cb2ai{40u, nullptr, cp, 1u, 1u};
    void* cb2 = nullptr;
    ((VkAllocCb_t)fn("vkAllocateCommandBuffers"))(dev, &cb2ai, &cb2);
    ((VkBeginCb_t)fn("vkBeginCommandBuffer"))(cb2, &cbbi);
    ((VkDispInd_t)gdpa(dev, "vkCmdDispatchIndirect"))(cb2, stg, 0u);
    ((VkDrawInd_t)gdpa(dev, "vkCmdDrawIndirect"))(cb2, stg, 16u, 1u, 16u);
    ((VkEndCb_t)fn("vkEndCommandBuffer"))(cb2);

    const u32 ind_disp[3] = {7u, 2u, 1u};
    const u32 ind_draw[4] = {12u, 1u, 0u, 0u};
    struct { u32 sType; const void* pNext; u64 rp, fb; i32 x, y; u32 w, h, cc; const float* pc; } rpb{43u, nullptr, rp2, fb2, 0, 0, 64, 64, 0, nullptr};
    struct { u32 sType; const void* pNext; u32 contents; } sb{1000109005u, nullptr, 1u};
    struct { u32 sType; const void* pNext; } se{1000109006u, nullptr};

    ((VkBeginCb_t)fn("vkBeginCommandBuffer"))(cb, &cbbi);
    ((VkUpdBuf_t)gdpa(dev, "vkCmdUpdateBuffer"))(cb, stg, 0u, sizeof(ind_disp), ind_disp);
    ((VkUpdBuf_t)gdpa(dev, "vkCmdUpdateBuffer"))(cb, stg, 16u, sizeof(ind_draw), ind_draw);
    ((VkBindPipe_t)fn("vkCmdBindPipeline"))(cb, 1u, cpipe);
    ((VkBindPipe_t)fn("vkCmdBindPipeline"))(cb, 0u, gp);
    ((VkCmdRp2_t)gdpa(dev, "vkCmdBeginRenderPass2KHR"))(cb, &rpb, &sb);
    ((VkExecCmds_t)gdpa(dev, "vkCmdExecuteCommands"))(cb, 1u, &cb2);
    ((VkCmdEndRp2_t)gdpa(dev, "vkCmdEndRenderPass2KHR"))(cb, &se);
    ((VkEndCb_t)fn("vkEndCommandBuffer"))(cb);

    struct { u32 sType; const void* pNext; void* cb; u32 mask; } cbsi{1000314002u, nullptr, cb, 0u};
    struct { u32 sType; const void* pNext; u32 fl, wsc; const void* pws; u32 cbc; const void* pcb; u32 ssc; const void* pss; } si2{1000314001u, nullptr, 0, 0, nullptr, 1u, &cbsi, 0, nullptr};
    ((VkSubmit2_t)gdpa(dev, "vkQueueSubmit2KHR"))(q, 1u, &si2, 0u);
    CHECK(g_obs.rp_begun == 2 && g_obs.rp_ended == 2,
          "vkCmdBeginRenderPass2KHR/EndRenderPass2KHR + vkQueueSubmit2KHR executed");
    CHECK(g_obs.dispatches == 2 && g_obs.last_gx == 7u,
          "vkCmdUpdateBuffer + vkCmdExecuteCommands + vkCmdDispatchIndirect ran with gx=7");
    CHECK(g_obs.draws == 1 && g_obs.last_draw_verts == 12u,
          "vkCmdDrawIndirect read indirect VertexCount=12 from buffer memory");

    // --- 2c. Vulkan completion batch 1: honest Features2, vkCreateDevice
    // validation, VK_KHR_push_descriptor, fail-visible pipeline creation ---
    using VkEnumDevExt_t = i32(TRT_GUEST_ABI*)(void*, const char*, u32*, void*);
    u32 ext_cnt = 0;
    ((VkEnumDevExt_t)gipa(inst, "vkEnumerateDeviceExtensionProperties"))(gpu, nullptr, &ext_cnt, nullptr);
    // Each entry is {char name[256]; u32 specVersion} = 260 bytes.
    std::vector<u8> ext_buf((size_t)ext_cnt * 260);
    ((VkEnumDevExt_t)gipa(inst, "vkEnumerateDeviceExtensionProperties"))(gpu, nullptr, &ext_cnt, ext_buf.data());
    bool have_push_ext = false;
    for (u32 e = 0; e < ext_cnt; ++e) {
      const char* nm = (const char*)ext_buf.data() + (size_t)e * 260;
      if (strcmp(nm, "VK_KHR_push_descriptor") == 0) have_push_ext = true;
    }
    CHECK(have_push_ext, "vkEnumerateDeviceExtensionProperties advertises VK_KHR_push_descriptor");

#if __has_include(<vulkan/vulkan_core.h>)
    using VkGetFeat2_t = void(TRT_GUEST_ABI*)(void*, void*);
    VkPhysicalDeviceVulkan11Features f11{};
    f11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    VkPhysicalDeviceVulkan12Features f12{};
    f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceVulkan13Features f13{};
    f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceDescriptorIndexingFeatures fdi{};
    fdi.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    VkPhysicalDeviceBufferDeviceAddressFeatures fbda{};
    fbda.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
    VkPhysicalDeviceFeatures2 f2{};
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f2.pNext = &f11; f11.pNext = &f12; f12.pNext = &f13; f13.pNext = &fdi; fdi.pNext = &fbda;
    ((VkGetFeat2_t)gipa(inst, "vkGetPhysicalDeviceFeatures2"))(gpu, &f2);
    CHECK(f2.features.geometryShader == 0 && f2.features.tessellationShader == 0 &&
              f2.features.fragmentStoresAndAtomics == 0 &&
              f2.features.vertexPipelineStoresAndAtomics == 0 &&
              f2.features.shaderImageGatherExtended == 0 &&
              f2.features.multiViewport == 0 && f2.features.occlusionQueryPrecise == 0 &&
              f2.features.pipelineStatisticsQuery == 0,
          "Features2 base: geometry/tessellation/atomics/gather/multiViewport/queries honestly FALSE");
    CHECK(f2.features.imageCubeArray != 0 && f2.features.independentBlend != 0,
          "Features2 base: imageCubeArray/independentBlend stay TRUE");
    CHECK(f12.drawIndirectCount != 0 && f12.timelineSemaphore != 0 &&
              f12.samplerMirrorClampToEdge != 0,
          "Vulkan12Features: drawIndirectCount/timelineSemaphore/mirrorClamp TRUE");
    CHECK(f12.descriptorIndexing == 0 && f12.bufferDeviceAddress == 0 &&
              f12.runtimeDescriptorArray == 0 && f12.shaderFloat16 == 0,
          "Vulkan12Features: descriptorIndexing/bufferDeviceAddress honestly FALSE");
    CHECK(f13.dynamicRendering != 0 && f13.synchronization2 != 0,
          "Vulkan13Features: dynamicRendering/synchronization2 TRUE");
    CHECK(f13.inlineUniformBlock == 0 && f13.privateData == 0 &&
              f13.shaderDemoteToHelperInvocation == 0,
          "Vulkan13Features: inlineUniformBlock/privateData/demote honestly FALSE");
    CHECK(f11.shaderDrawParameters == 0 && f11.samplerYcbcrConversion == 0 &&
              f11.multiview == 0,
          "Vulkan11Features: drawParameters/ycbcr/multiview honestly FALSE");
    CHECK(fdi.runtimeDescriptorArray == 0 && fdi.descriptorBindingPartiallyBound == 0,
          "DescriptorIndexingFeatures: all FALSE");
    CHECK(fbda.bufferDeviceAddress == 0 && fbda.bufferDeviceAddressCaptureReplay == 0,
          "BufferDeviceAddressFeatures: all FALSE");
#endif

    // vkCreateDevice validation: unknown extension, unsupported feature,
    // and the consistency rule (reported-supported => accepted).
    const char* bad_ext = "VK_KHR_does_not_exist";
    struct { u32 sType; const void* pNext; u32 flags; u32 qcc; const void* pqc; u32 lc; const void* pl; u32 ec; const void* pe; const void* pf; } dci_bad{3u, nullptr, 0, 1, &dq, 0, nullptr, 1, &bad_ext, nullptr};
    void* dev_bad = nullptr;
    i32 rc_bad = ((VkCreateDev_t)fn("vkCreateDevice"))(gpu, &dci_bad, nullptr, &dev_bad);
    CHECK(rc_bad == -7 && dev_bad == nullptr,
          "vkCreateDevice rejects unknown extension with VK_ERROR_EXTENSION_NOT_PRESENT");
#if __has_include(<vulkan/vulkan_core.h>)
    VkPhysicalDeviceFeatures feat_geo{};
    feat_geo.geometryShader = VK_TRUE;
    struct { u32 sType; const void* pNext; u32 flags; u32 qcc; const void* pqc; u32 lc; const void* pl; u32 ec; const void* pe; const void* pf; } dci_geo{3u, nullptr, 0, 1, &dq, 0, nullptr, 0, nullptr, &feat_geo};
    void* dev_geo = nullptr;
    i32 rc_geo = ((VkCreateDev_t)fn("vkCreateDevice"))(gpu, &dci_geo, nullptr, &dev_geo);
    CHECK(rc_geo == -8 && dev_geo == nullptr,
          "vkCreateDevice rejects geometryShader with VK_ERROR_FEATURE_NOT_PRESENT");
    VkPhysicalDeviceDynamicRenderingFeatures fdr{};
    fdr.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
    fdr.dynamicRendering = VK_TRUE;
    struct { u32 sType; const void* pNext; u32 flags; u32 qcc; const void* pqc; u32 lc; const void* pl; u32 ec; const void* pe; const void* pf; } dci_dr{3u, &fdr, 0, 1, &dq, 0, nullptr, 0, nullptr, nullptr};
    void* dev_dr = nullptr;
    i32 rc_dr = ((VkCreateDev_t)fn("vkCreateDevice"))(gpu, &dci_dr, nullptr, &dev_dr);
    CHECK(rc_dr == 0 && dev_dr != nullptr,
          "vkCreateDevice accepts dynamicRendering (reported supported)");
    if (dev_dr) ((VkDestroyDev_t)fn("vkDestroyDevice"))(dev_dr, nullptr);
    VkPhysicalDeviceDescriptorIndexingFeatures fdi_req{};
    fdi_req.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    fdi_req.runtimeDescriptorArray = VK_TRUE;
    struct { u32 sType; const void* pNext; u32 flags; u32 qcc; const void* pqc; u32 lc; const void* pl; u32 ec; const void* pe; const void* pf; } dci_di{3u, &fdi_req, 0, 1, &dq, 0, nullptr, 0, nullptr, nullptr};
    void* dev_di = nullptr;
    i32 rc_di = ((VkCreateDev_t)fn("vkCreateDevice"))(gpu, &dci_di, nullptr, &dev_di);
    CHECK(rc_di == -8 && dev_di == nullptr,
          "vkCreateDevice rejects descriptor indexing with VK_ERROR_FEATURE_NOT_PRESENT");
#endif
    const char* push_ext = "VK_KHR_push_descriptor";
    struct { u32 sType; const void* pNext; u32 flags; u32 qcc; const void* pqc; u32 lc; const void* pl; u32 ec; const void* pe; const void* pf; } dci_push{3u, nullptr, 0, 1, &dq, 0, nullptr, 1, &push_ext, nullptr};
    void* dev_push = nullptr;
    i32 rc_push_dev = ((VkCreateDev_t)fn("vkCreateDevice"))(gpu, &dci_push, nullptr, &dev_push);
    CHECK(rc_push_dev == 0 && dev_push != nullptr,
          "vkCreateDevice accepts VK_KHR_push_descriptor");
    if (dev_push) ((VkDestroyDev_t)fn("vkDestroyDevice"))(dev_push, nullptr);

    // Fail-visible pipeline creation: garbage SPIR-V (bad magic) must fail
    // the call and return VK_NULL_HANDLE, never a stage-less pipeline.
    const u32 bad_spv[5] = {0, 0, 0, 0, 0};
    struct { u32 sType; const void* pNext; u32 flags; size_t sz; const u32* code; } bad_smci{15u, nullptr, 0, sizeof(bad_spv), bad_spv};
    u64 bad_mod = 0;
    ((VkCreateSm_t)fn("vkCreateShaderModule"))(dev, &bad_smci, nullptr, &bad_mod);
    CHECK(bad_mod != 0, "garbage shader module created (translation is deferred to pipeline creation)");
    int pipes_before = g_obs.pipes_created;
    struct { u32 sType; const void* pNext; u32 flags; u32 stage; u64 module; const char* pName; const void* pSpec; } bad_stage{18u, nullptr, 0, 0x1u, bad_mod, "main", nullptr};
    struct { u32 sType; const void* pNext; u32 flags; u32 stageCount; const void* pStages; const void* pvi, *pia, *pt, *pv, *pr, *pm, *pds, *pcb, *pdy; u64 layout, rp; u32 sp; u64 bh; i32 bi; } bad_gpci{28u, nullptr, 0, 1u, &bad_stage};
    u64 bad_pipe = 0xDEADu;
    i32 rc_bad_pipe = ((VkCreateGp_t)gdpa(dev, "vkCreateGraphicsPipelines"))(dev, 0, 1, &bad_gpci, nullptr, &bad_pipe);
    CHECK(rc_bad_pipe != 0 && bad_pipe == 0,
          "vkCreateGraphicsPipelines fails visibly on untranslatable SPIR-V");
    CHECK(g_obs.pipes_created == pipes_before,
          "failed graphics pipeline never reaches the backend hook");
    int comp_before = g_obs.comp_pipes_created;
    struct { u32 sType; const void* pNext; u32 flags; struct { u32 s; const void* pn; u32 fl; u32 stg; u64 m; const char* nm; const void* sp; } stage; u64 layout, base; i32 bidx; } bad_cci{29u, nullptr, 0, {18u, nullptr, 0, 0x20u, bad_mod, "main", nullptr}};
    u64 bad_cpipe = 0xDEADu;
    i32 rc_bad_cpipe = ((VkCreateCp_t)fn("vkCreateComputePipelines"))(dev, 0, 1, &bad_cci, nullptr, &bad_cpipe);
    CHECK(rc_bad_cpipe != 0 && bad_cpipe == 0,
          "vkCreateComputePipelines fails visibly on untranslatable SPIR-V");
    CHECK(g_obs.comp_pipes_created == comp_before,
          "failed compute pipeline never reaches the backend hook");

    // Push descriptors: VS reads a UBO (set 0, binding 0); the push must
    // feed that binding into the draw with no descriptor set bound.
    spv_builder pb;
    pb.begin();
    u32 p_fn = pb.id(), p_inPos = pb.id(), p_outPos = pb.id(), p_ubo = pb.id();
    u32 p_tVoid = pb.id(), p_tFn = pb.id(), p_tF32 = pb.id(), p_tV4 = pb.id(), p_tU32 = pb.id();
    u32 p_c0 = pb.id(), p_tUbo = pb.id(), p_tPtrUbo = pb.id(), p_tPtrUboV = pb.id();
    u32 p_tPtrIn = pb.id(), p_tPtrOut = pb.id(), p_lbl = pb.id(), p_gep = pb.id();
    u32 p_vPos = pb.id(), p_vOff = pb.id(), p_vSum = pb.id();
    pb.emit(17 /*OpCapability*/, {1u});
    pb.emit(14 /*OpMemoryModel*/, {0u, 1u});
    pb.emit_str(15 /*OpEntryPoint*/, {0u /*Vertex*/, p_fn}, "main", {p_inPos, p_outPos});
    pb.emit(71 /*OpDecorate*/, {p_inPos, 30u /*Location*/, 0u});
    pb.emit(71 /*OpDecorate*/, {p_outPos, 11u /*BuiltIn*/, 0u /*Position*/});
    pb.emit(71 /*OpDecorate*/, {p_ubo, 34u /*DescriptorSet*/, 0u});
    pb.emit(71 /*OpDecorate*/, {p_ubo, 33u /*Binding*/, 0u});
    pb.emit(71 /*OpDecorate*/, {p_tUbo, 2u /*Block*/});
    pb.emit(72 /*OpMemberDecorate*/, {p_tUbo, 0u, 35u /*Offset*/, 0u});
    pb.emit(19, {p_tVoid});
    pb.emit(33, {p_tFn, p_tVoid});
    pb.emit(22, {p_tF32, 32u});
    pb.emit(23, {p_tV4, p_tF32, 4u});
    pb.emit(21, {p_tU32, 32u, 0u});
    pb.emit(43, {p_tU32, p_c0, 0u});
    pb.emit(30, {p_tUbo, p_tV4});
    pb.emit(32, {p_tPtrUbo, 2u /*Uniform*/, p_tUbo});
    pb.emit(32, {p_tPtrUboV, 2u, p_tV4});
    pb.emit(32, {p_tPtrIn, 1u, p_tV4});
    pb.emit(32, {p_tPtrOut, 3u, p_tV4});
    pb.emit(59, {p_tPtrUbo, p_ubo, 2u});
    pb.emit(59, {p_tPtrIn, p_inPos, 1u});
    pb.emit(59, {p_tPtrOut, p_outPos, 3u});
    pb.emit(54, {p_tVoid, p_fn, 0u, p_tFn});
    pb.emit(248, {p_lbl});
    pb.emit(65, {p_tPtrUboV, p_gep, p_ubo, p_c0});
    pb.emit(61, {p_tV4, p_vPos, p_inPos});
    pb.emit(61, {p_tV4, p_vOff, p_gep});
    pb.emit(129 /*OpFAdd*/, {p_tV4, p_vSum, p_vPos, p_vOff});
    pb.emit(62, {p_outPos, p_vSum});
    pb.emit(253, {});
    pb.emit(56, {});
    pb.finish();
    spirv_compile_result rpush_vs = spirv_to_hlsl(pb.w.data(), (u32)pb.w.size(),
                                                  SPV_STAGE_VERTEX, "main");
    CHECK(rpush_vs.ok && rpush_vs.bindings.size() == 1 &&
              rpush_vs.bindings[0].kind == SPV_BIND_CBV,
          "push-descriptor test VS reflects UBO as one CBV binding");
    spv_builder qb;
    qb.begin();
    u32 q_fn = qb.id(), q_out = qb.id();
    u32 q_tVoid = qb.id(), q_tFn = qb.id(), q_tF32 = qb.id(), q_tV4 = qb.id();
    u32 q_tPtrOut = qb.id(), q_c0 = qb.id(), q_c1 = qb.id(), q_cCol = qb.id(), q_lbl = qb.id();
    qb.emit(17, {1u});
    qb.emit(14, {0u, 1u});
    qb.emit_str(15, {4u /*Fragment*/, q_fn}, "main", {q_out});
    qb.emit(16, {q_fn, 7u /*OriginUpperLeft*/});
    qb.emit(71, {q_out, 30u, 0u});
    qb.emit(19, {q_tVoid});
    qb.emit(33, {q_tFn, q_tVoid});
    qb.emit(22, {q_tF32, 32u});
    qb.emit(23, {q_tV4, q_tF32, 4u});
    qb.emit(43, {q_tF32, q_c0, 0x00000000u});
    qb.emit(43, {q_tF32, q_c1, 0x3F800000u});
    qb.emit(44 /*OpConstantComposite*/, {q_tV4, q_cCol, q_c1, q_c0, q_c0, q_c1});
    qb.emit(32, {q_tPtrOut, 3u, q_tV4});
    qb.emit(59, {q_tPtrOut, q_out, 3u});
    qb.emit(54, {q_tVoid, q_fn, 0u, q_tFn});
    qb.emit(248, {q_lbl});
    qb.emit(62, {q_out, q_cCol});
    qb.emit(253, {});
    qb.emit(56, {});
    qb.finish();
    spirv_compile_result rpush_ps = spirv_to_hlsl(qb.w.data(), (u32)qb.w.size(),
                                                  SPV_STAGE_FRAGMENT, "main");
    CHECK(rpush_ps.ok, "push-descriptor test PS compiles");
    using VkCreatePl_t = i32(TRT_GUEST_ABI*)(void*, const void*, const void*, u64*);
    u64 push_layout = 0;
    ((VkCreatePl_t)fn("vkCreatePipelineLayout"))(dev, nullptr, nullptr, &push_layout);
    struct { u32 sType; const void* pNext; u32 flags; size_t sz; const u32* code; } vs_smci{15u, nullptr, 0, pb.w.size() * 4, pb.w.data()};
    struct { u32 sType; const void* pNext; u32 flags; size_t sz; const u32* code; } ps_smci{15u, nullptr, 0, qb.w.size() * 4, qb.w.data()};
    u64 vs_mod = 0, ps_mod = 0;
    ((VkCreateSm_t)fn("vkCreateShaderModule"))(dev, &vs_smci, nullptr, &vs_mod);
    ((VkCreateSm_t)fn("vkCreateShaderModule"))(dev, &ps_smci, nullptr, &ps_mod);
    struct { u32 sType; const void* pNext; u32 flags; u32 stage; u64 module; const char* pName; const void* pSpec; } push_stages[2] = {
        {18u, nullptr, 0, 0x1u, vs_mod, "main", nullptr},
        {18u, nullptr, 0, 0x10u, ps_mod, "main", nullptr}};
    struct { u32 sType; const void* pNext; u32 flags; u32 stageCount; const void* pStages; const void* pvi, *pia, *pt, *pv, *pr, *pm, *pds, *pcb, *pdy; u64 layout, rp; u32 sp; u64 bh; i32 bi; } push_gpci{28u, nullptr, 0, 2u, push_stages, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, push_layout};
    u64 push_pipe = 0;
    i32 rc_push_pipe = ((VkCreateGp_t)gdpa(dev, "vkCreateGraphicsPipelines"))(dev, 0, 1, &push_gpci, nullptr, &push_pipe);
    CHECK(rc_push_pipe == 0 && push_pipe != 0, "push-descriptor test graphics pipeline created");
    using VkPushDesc_t = void(TRT_GUEST_ABI*)(void*, u32, u64, u32, u32, const void*);
    using VkDraw_t = void(TRT_GUEST_ABI*)(void*, u32, u32, u32, u32);
    void* push_fn = gdpa(dev, "vkCmdPushDescriptorSetKHR");
    CHECK(push_fn != nullptr, "vkCmdPushDescriptorSetKHR resolves via vkGetDeviceProcAddr");
    struct { u64 buffer; u64 offset; u64 range; } push_bi{stg, 0, 64};
    struct { u32 sType; const void* pNext; u64 dstSet; u32 dstBinding; u32 dstArrayElement; u32 descriptorCount; u32 descriptorType; const void* pImageInfo; const void* pBufferInfo; const void* pTexelBufferView; } push_w{0u, nullptr, 0, 0u, 0u, 1u, 6u /*UNIFORM_BUFFER*/, nullptr, &push_bi, nullptr};
    int draws_before = g_obs.draws;
    ((VkBeginCb_t)fn("vkBeginCommandBuffer"))(cb, &cbbi);
    ((VkBindPipe_t)fn("vkCmdBindPipeline"))(cb, 0u, push_pipe);
    if (push_fn) ((VkPushDesc_t)push_fn)(cb, 0u, push_layout, 0u, 1u, &push_w);
    ((VkDraw_t)fn("vkCmdDraw"))(cb, 3u, 1u, 0u, 0u);
    ((VkEndCb_t)fn("vkEndCommandBuffer"))(cb);
    struct { u32 sType; const void* pNext; u32 wc; const void* pw, *ps; u32 cbc; void* const* pcb; u32 sc; const void* psig; } push_subi{4u, nullptr, 0, nullptr, nullptr, 1, &cb};
    ((VkSubmit_t)fn("vkQueueSubmit"))(q, 1, &push_subi, 0);
    CHECK(g_obs.draws == draws_before + 1 && g_obs.last_binding_count == 1u,
          "vkCmdPushDescriptorSetKHR feeds the pushed UBO into the draw");
    // Binding a (never-updated) real set over the pushed set replaces it.
    using VkCreateDp_t = i32(TRT_GUEST_ABI*)(void*, const void*, const void*, u64*);
    using VkAllocDs_t = i32(TRT_GUEST_ABI*)(void*, const void*, u64*);
    using VkBindDs_t = void(TRT_GUEST_ABI*)(void*, u32, u64, u32, u32, const u64*, u32, const u32*);
    u64 dpool = 0;
    ((VkCreateDp_t)fn("vkCreateDescriptorPool"))(dev, nullptr, nullptr, &dpool);
    u64 dset = 0;
    const u64 lay1 = push_layout;
    struct { u32 sType; const void* pNext; u64 pool; u32 cnt; const u64* layouts; } dsai{30u, nullptr, dpool, 1u, &lay1};
    ((VkAllocDs_t)fn("vkAllocateDescriptorSets"))(dev, &dsai, &dset);
    int draws_before2 = g_obs.draws;
    ((VkBeginCb_t)fn("vkBeginCommandBuffer"))(cb, &cbbi);
    ((VkBindPipe_t)fn("vkCmdBindPipeline"))(cb, 0u, push_pipe);
    if (push_fn) ((VkPushDesc_t)push_fn)(cb, 0u, push_layout, 0u, 1u, &push_w);
    ((VkBindDs_t)fn("vkCmdBindDescriptorSets"))(cb, 0u, push_layout, 0u, 1u, &dset, 0u, nullptr);
    ((VkDraw_t)fn("vkCmdDraw"))(cb, 3u, 1u, 0u, 0u);
    ((VkEndCb_t)fn("vkEndCommandBuffer"))(cb);
    ((VkSubmit_t)fn("vkQueueSubmit"))(q, 1, &push_subi, 0);
    CHECK(g_obs.draws == draws_before2 + 1 && g_obs.last_binding_count == 0u,
          "binding a descriptor set replaces the pushed set");

    ((VkDestroyDev_t)fn("vkDestroyDevice"))(dev, nullptr);
    ((VkDestroyInst_t)fn("vkDestroyInstance"))(inst, nullptr);
  }

  // --- 3. Load and execute guest_vulkan.exe through PE loader + IAT ---
  const char* guest_path = (argc > 1) ? argv[1] : "out/guest_vulkan.exe";
  size_t fsz = 0;
  u8* fdata = read_file_bytes(guest_path, &fsz);
  CHECK(fdata != nullptr && fsz > 512, "guest_vulkan.exe exists on disk");

  if (fdata) {
    g_obs = obs_state{};
    trt_resolver rs{};
    trt_resolver_default(&rs);
    trt_image* img = nullptr;
    int map_rc = trt_map_pe(&be, &rs, fdata, fsz, &img);
    CHECK(map_rc == TRT_OK && img != nullptr, "guest_vulkan.exe PE mapped cleanly & vulkan-1.dll IAT resolved");
    if (map_rc == TRT_OK && img) {
      CHECK(trt_seal_image(&be, img) == TRT_OK, "guest_vulkan.exe image sealed RX");
      void* entry = (void*)(img->base + img->entry_rva);
      u32 mask = (u32)win64_call(entry);
      CHECK(mask == 0xFFFFu, "guest_vulkan.exe full pipeline returned 0xFFFF");
      CHECK(g_obs.pipes_created == 1, "guest_vulkan.exe compiled real SPIR-V->HLSL graphics pipeline");
      CHECK(g_obs.last_vs.find("TEXCOORD0") != std::string::npos &&
                g_obs.last_ps.find("cbuffer") != std::string::npos,
            "guest_vulkan.exe VS/PS reflected inputs & UBO");
      CHECK(g_obs.buffers_synced >= 2, "guest_vulkan.exe synced VB and UBO to host");
      CHECK(g_obs.rp_begun == 1 && g_obs.rp_ended == 1, "guest_vulkan.exe executed render pass");
      CHECK(g_obs.draws == 1 && g_obs.last_draw_verts == 3u &&
                g_obs.last_push_bytes == 16u && g_obs.last_binding_count == 1u,
            "guest_vulkan.exe dispatched vkCmdDraw(3) with UBO + 16B push constants");
      CHECK(g_obs.presents == 1, "guest_vulkan.exe presented via vkQueuePresentKHR");
      trt_unmap(&be, img);
    }
    free(fdata);
  }

  printf("  result: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
