// tiramisu/src/trt/bridge_d3d12.cpp
// Direct3D 12 bridge (d3d12.dll + d3d12core.dll).
//   - On Xbox Series Dev Mode (UWP): forwards directly to the console's
//     hardware D3D12 runtime once hooks are bound by host swapchain.
//   - Offline / Linux CI: full stateful software D3D12 COM runtime using exact
//     MSVC Win64 vtable indices and hidden struct-return parameters.
#include "bridge_d3d12.h"
#include "thunk.h"

#include <atomic>
#include <mutex>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unordered_set>
#include <vector>

extern "C" int TRT_STDCALL trt_SetEvent(u64 handle);

namespace {

trt_d3d12_hooks g_hooks{};

constexpr int k_s_ok = 0;
constexpr int k_s_false = 1;
constexpr int k_e_nointerface = (int)0x80004002u;
constexpr int k_e_invalidarg = (int)0x80070057u;
constexpr int k_e_outofmemory = (int)0x8007000Eu;
constexpr int k_e_notimpl = (int)0x80004001u;
constexpr int k_dxgi_error_unsupported = (int)0x887A0004u;

constexpr u32 k_fl_9_1 = 0x9100;
constexpr u32 k_fl_11_0 = 0xb000;
constexpr u32 k_fl_11_1 = 0xb100;
constexpr u32 k_fl_12_0 = 0xc000;
constexpr u32 k_fl_12_1 = 0xc100;
constexpr u32 k_fl_12_2 = 0xc200;

struct alignas(4) raw_guid {
  u32 d1;
  u16 d2;
  u16 d3;
  u8 d4[8];
};

bool guid_eq(const void* g, u32 d1, u16 d2, u16 d3,
             u8 b0, u8 b1, u8 b2, u8 b3, u8 b4, u8 b5, u8 b6, u8 b7) {
  if (!g) return false;
  const raw_guid* r = (const raw_guid*)g;
  return r->d1 == d1 && r->d2 == d2 && r->d3 == d3 &&
         r->d4[0] == b0 && r->d4[1] == b1 && r->d4[2] == b2 && r->d4[3] == b3 &&
         r->d4[4] == b4 && r->d4[5] == b5 && r->d4[6] == b6 && r->d4[7] == b7;
}

bool is_iunknown(const void* riid) {
  return guid_eq(riid, 0x00000000u, 0x0000, 0x0000, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
}

// ---- BUG-6: version-aware QueryInterface ----------------------------------
// IID values verified verbatim against the xwin SDK d3d12.h DEFINE_GUID block
// (the same constants the console's d3d12.dll ships). A vtbl serves exactly
// the interface versions whose slot count it covers; anything else fails
// VISIBLY with E_NOINTERFACE instead of handing back a short vtbl that a
// game would call off the end of.
struct qi_iid {
  u32 d1;
  u16 d2, d3;
  u8 d4[8];
  const char* name;
};

#define TIRAMISU_IID(sym, a, b, c, b0, b1, b2, b3, b4, b5, b6, b7) \
  { a, b, c, { b0, b1, b2, b3, b4, b5, b6, b7 }, #sym }

static const qi_iid k_iid_object = TIRAMISU_IID(ID3D12Object,
    0xc4fec28fu, 0x7966, 0x4e95, 0x9f, 0x94, 0xf4, 0x31, 0xcb, 0x56, 0xc3, 0xb8);
static const qi_iid k_iid_device_child = TIRAMISU_IID(ID3D12DeviceChild,
    0x905db94bu, 0xa00c, 0x4140, 0x9d, 0xf5, 0x2b, 0x64, 0xca, 0x9e, 0xa3, 0x57);
static const qi_iid k_iid_pageable = TIRAMISU_IID(ID3D12Pageable,
    0x63ee58fbu, 0x1268, 0x4835, 0x86, 0xda, 0xf0, 0x08, 0xce, 0x62, 0xf0, 0xd6);

// chain is a nullptr-terminated array of accepted IIDs (IUnknown implied).
u32 TRT_GUEST_ABI base_addref(void* self);  // defined below with com_base
static int d3d12_qi_chain(void* self, const void* riid, void** ppv,
                          const qi_iid* const* chain, const char* what) {
  if (!ppv) return k_e_invalidarg;
  *ppv = nullptr;
  if (!riid) return k_e_invalidarg;
  if (is_iunknown(riid)) {
    *ppv = self;
    base_addref(self);
    return k_s_ok;
  }
  for (u32 i = 0; chain[i]; ++i) {
    const qi_iid& e = *chain[i];
    if (guid_eq(riid, e.d1, e.d2, e.d3, e.d4[0], e.d4[1], e.d4[2], e.d4[3],
                e.d4[4], e.d4[5], e.d4[6], e.d4[7])) {
      *ppv = self;
      base_addref(self);
      return k_s_ok;
    }
  }
  const raw_guid* r = (const raw_guid*)riid;
  char line[160];
  snprintf(line, sizeof(line),
           "d3d12: %s QueryInterface({%08x-%04x-%04x-%02x%02x...}) -> "
           "E_NOINTERFACE (interface version beyond bridge vtbl)",
           what, r->d1, r->d2, r->d3, r->d4[0], r->d4[1]);
  trt_core_log(line);
  return k_e_nointerface;
}

// log-once gate for the versioned command-list stubs (24 methods beyond the
// ID3D12GraphicsCommandList base slots) — visible on first use, silent after
// so per-frame DXR/VRS calls cannot flood the log.
static std::atomic<u32> g_gcl_stub_seen{0};
static void gcl_stub_log(u32 bit, const char* name) {
  u32 seen = g_gcl_stub_seen.load(std::memory_order_relaxed);
  if (!(seen & (1u << bit)) &&
      !(g_gcl_stub_seen.fetch_or(1u << bit, std::memory_order_relaxed) & (1u << bit))) {
    char line[160];
    snprintf(line, sizeof(line),
             "d3d12: GraphicsCommandList::%s -> logged no-op "
             "(versioned slot; real impl = sprint-2 item 27/34)", name);
    trt_core_log(line);
  }
}

bool is_valid_fl(u32 fl) {
  return fl == k_fl_9_1 || fl == 0x9200 || fl == 0x9300 ||
         fl == 0xa000 || fl == 0xa100 || fl == k_fl_11_0 ||
         fl == k_fl_11_1 || fl == k_fl_12_0 || fl == k_fl_12_1 || fl == k_fl_12_2;
}

std::mutex g_track_mu;
std::unordered_set<void*> g_queues;
std::atomic<u64> g_next_va{0x120000000ULL};
std::atomic<u64> g_next_desc{0x240000000ULL};

// ---- Sprint-2 live-object registries (descriptor heaps / resources / rootsigs)
// A CPU descriptor handle only resolves to (heap, index) if the heap is live;
// a view only validates against a resource the bridge actually created; a PSO
// root-signature pointer must reference a root signature CreateRootSignature
// returned. Everything else fails VISIBLY with a named log line.
std::mutex g_live_mu;
std::unordered_set<void*> g_live_heaps;
std::unordered_set<void*> g_live_resources;
std::unordered_set<void*> g_live_rootsigs;
std::unordered_set<void*> g_live_psos;
std::unordered_set<void*> g_live_cmdlists;

// log-once gate for recording-level success lines / soft warnings (the
// fail-visible E_INVALIDARG lines log EVERY time — garbage is rare and each
// occurrence is diagnostic gold; success lines log once so per-frame calls
// cannot flood the log).
static std::atomic<u32> g_d3d12_once_seen{0};
static void d3d12_log_once(u32 bit, const char* line) {
  const u32 mask = 1u << bit;
  const u32 seen = g_d3d12_once_seen.load(std::memory_order_relaxed);
  if (!(seen & mask) &&
      !(g_d3d12_once_seen.fetch_or(mask, std::memory_order_relaxed) & mask)) {
    trt_core_log(line);
  }
}

// Forward decls: the fail-visible logger + liveness/resolve helpers are
// defined in the view/copy block below, but the command-list and resource
// sections above need them too.
struct d3d12_desc_heap;
static bool is_live_resource(const void* p);
static bool is_live_root_sig(const void* p);
static bool is_live_pso(const void* p);
static bool is_live_cmdlist(const void* p);
static d3d12_desc_heap* resolve_gpu_handle(u64 h, u32* out_idx,
                                           const char* api, char* reason,
                                           size_t reason_sz);
static void view_fail(const char* api, const char* reason);

// ---- ABI POD layouts matching xwin d3d12.h on x86_64 -----------------------
struct d3d12_luid_t {
  u32 low;
  i32 high;
};

struct d3d12_queue_desc_t {
  u32 type;
  i32 priority;
  u32 flags;
  u32 node_mask;
};

struct d3d12_heap_desc_info_t {
  u32 type;
  u32 num_descriptors;
  u32 flags;
  u32 node_mask;
};

struct d3d12_heap_props_t {
  u32 type;
  u32 cpu_page_prop;
  u32 memory_pool;
  u32 creation_node_mask;
  u32 visible_node_mask;
};

struct d3d12_heap_desc_t {
  u64 size_in_bytes;
  d3d12_heap_props_t props;
  u64 alignment;
  u32 flags;
  u32 pad;
};

struct d3d12_resource_desc_t {
  u32 dimension;
  u32 pad0;
  u64 alignment;
  u64 width;
  u32 height;
  u16 depth_or_array;
  u16 mip_levels;
  u32 format;
  u32 sample_count;
  u32 sample_quality;
  u32 layout;
  u32 flags;
  u32 pad1;
};

struct d3d12_alloc_info_t {
  u64 size_in_bytes;
  u64 alignment;
};

struct d3d12_placed_footprint_t {
  u64 offset;
  u32 format;
  u32 width;
  u32 height;
  u32 depth;
  u32 row_pitch;
};

// ---- Sprint-2: byte-exact mirrors of the guest-visible descriptor/PSO structs
// (sizes+offsets compiler-verified against the xwin SDK d3d12.h under the
// Win64 ABI; see the wave-5 ABI probe). Every guest-callable parse reads
// through these layouts — a wrong offset reads garbage, so each is pinned.
struct d3d12_cbv_desc_t {          // D3D12_CONSTANT_BUFFER_VIEW_DESC
  u64 buffer_location;             // D3D12_GPU_VIRTUAL_ADDRESS
  u32 size_in_bytes;
};
static_assert(sizeof(d3d12_cbv_desc_t) == 16, "CBV desc must be 16 bytes");

struct d3d12_srv_desc_t {          // D3D12_SHADER_RESOURCE_VIEW_DESC (40)
  u32 format;                      // DXGI_FORMAT
  u32 view_dimension;              // D3D12_SRV_DIMENSION
  u32 shader4_component_mapping;
  u32 pad;
  u8 payload[24];                  // union (largest arm 24: TEX2D_ARRAY/BUFFER)
};
static_assert(sizeof(d3d12_srv_desc_t) == 40, "SRV desc must be 40 bytes");

struct d3d12_uav_desc_t {          // D3D12_UNORDERED_ACCESS_VIEW_DESC (40)
  u32 format;
  u32 view_dimension;              // D3D12_UAV_DIMENSION
  u8 payload[32];                  // union (largest arm 32: D3D12_BUFFER_UAV)
};
static_assert(sizeof(d3d12_uav_desc_t) == 40, "UAV desc must be 40 bytes");

struct d3d12_rtv_desc_t {          // D3D12_RENDER_TARGET_VIEW_DESC (24)
  u32 format;
  u32 view_dimension;              // D3D12_RTV_DIMENSION
  u8 payload[16];                  // union (largest arm 16: TEX2D_ARRAY_RTV)
};
static_assert(sizeof(d3d12_rtv_desc_t) == 24, "RTV desc must be 24 bytes");

struct d3d12_dsv_desc_t {          // D3D12_DEPTH_STENCIL_VIEW_DESC (24)
  u32 format;
  u32 view_dimension;              // D3D12_DSV_DIMENSION
  u32 flags;                       // D3D12_DSV_FLAGS
  u8 payload[12];                  // union at offset 12, align-4 (max arm 12)
};
static_assert(sizeof(d3d12_dsv_desc_t) == 24, "DSV desc must be 24 bytes");

struct d3d12_sampler_desc_t {      // D3D12_SAMPLER_DESC (52)
  u32 filter;                      // D3D12_FILTER (max valid 0x1d5)
  u32 address_u, address_v, address_w;
  float mip_lod_bias;
  u32 max_anisotropy;
  u32 comparison_func;             // D3D12_COMPARISON_FUNC (1..8)
  float border_color[4];
  float min_lod;
  float max_lod;
};
static_assert(sizeof(d3d12_sampler_desc_t) == 52, "SAMPLER desc must be 52 bytes");

struct d3d12_shader_bytecode_t {   // D3D12_SHADER_BYTECODE (16)
  const void* p_shader_bytecode;
  u64 bytecode_length;
};
static_assert(sizeof(d3d12_shader_bytecode_t) == 16, "shader bytecode 16");

struct d3d12_stream_output_desc_t {  // D3D12_STREAM_OUTPUT_DESC (32)
  const void* p_so_declaration;      // @0
  u32 num_entries;                   // @8
  u32 pad0;                          // @12
  const void* p_buffer_strides;      // @16
  u32 num_strides;                   // @24
  u32 rasterized_stream;             // @28
};
static_assert(sizeof(d3d12_stream_output_desc_t) == 32, "stream output desc 32");

struct d3d12_gpsd_t {              // D3D12_GRAPHICS_PIPELINE_STATE_DESC (656)
  void* p_root_signature;          // @0
  d3d12_shader_bytecode_t vs;      // @8
  d3d12_shader_bytecode_t ps;      // @24
  d3d12_shader_bytecode_t ds;      // @40
  d3d12_shader_bytecode_t hs;      // @56
  d3d12_shader_bytecode_t gs;      // @72
  d3d12_stream_output_desc_t stream_output;  // @88
  u8 blend_state[328];             // @120 D3D12_BLEND_DESC
  u32 sample_mask;                 // @448
  u8 rasterizer_state[44];         // @452 D3D12_RASTERIZER_DESC
  u8 depth_stencil_state[56];      // @496 D3D12_DEPTH_STENCIL_DESC
  const void* p_input_elements;    // @552
  u32 input_element_count;         // @560
  u32 input_pad;                   // @564
  u32 ib_strip_cut_value;          // @568
  u32 primitive_topology_type;     // @572
  u32 num_render_targets;          // @576
  u32 rtv_formats[8];              // @580
  u32 dsv_format;                  // @612
  u32 sample_count;                // @616
  u32 sample_quality;              // @620
  u32 node_mask;                   // @624
  u32 pad0;                        // @628
  const void* cached_blob;         // @632
  u64 cached_blob_size;            // @640
  u32 flags;                       // @648
  u32 pad1;                        // @652
};
static_assert(sizeof(d3d12_gpsd_t) == 656, "GRAPHICS PSO desc must be 656 bytes");

struct d3d12_cpsd_t {              // D3D12_COMPUTE_PIPELINE_STATE_DESC (56)
  void* p_root_signature;          // @0
  d3d12_shader_bytecode_t cs;      // @8
  u32 node_mask;                   // @24
  u32 pad0;                        // @28
  const void* cached_blob;         // @32
  u64 cached_blob_size;            // @40
  u32 flags;                       // @48
  u32 pad1;                        // @52
};
static_assert(sizeof(d3d12_cpsd_t) == 56, "COMPUTE PSO desc must be 56 bytes");

struct d3d12_stream_desc_t {       // D3D12_PIPELINE_STATE_STREAM_DESC (16)
  u64 size_in_bytes;               // @0
  void* p_subobject_stream;        // @8
};
static_assert(sizeof(d3d12_stream_desc_t) == 16, "stream desc 16");

struct d3d12_subobject_hdr_t {     // D3D12_PIPELINE_STATE_STREAM_SUBOBJECT header
  u32 type;                        // D3D12_PIPELINE_STATE_SUBOBJECT_TYPE @0
  u32 pad;                         // @4 (x64 alignment: SIZE_T size at 8)
  u64 size;                        // payload size in bytes @8
};
static_assert(sizeof(d3d12_subobject_hdr_t) == 16, "subobject header 16");

// D3D12_PIPELINE_STATE_SUBOBJECT_TYPE values (SDK-verbatim).
enum : u32 {
  k_subo_root_signature = 0,
  k_subo_vs = 1,
  k_subo_ps = 2,
  k_subo_ds = 3,
  k_subo_hs = 4,
  k_subo_gs = 5,
  k_subo_cs = 6,
  k_subo_stream_output = 7,
  k_subo_blend = 8,
  k_subo_sample_mask = 9,
  k_subo_rasterizer = 10,
  k_subo_depth_stencil = 11,
  k_subo_input_layout = 12,
  k_subo_ib_strip_cut = 13,
  k_subo_primitive_topology = 14,
  k_subo_render_target_formats = 15,
  k_subo_depth_stencil_format = 16,
  k_subo_sample_desc = 17,
  k_subo_node_mask = 18,
  k_subo_cached_pso = 19,
  k_subo_flags = 20,
  k_subo_depth_stencil1 = 21,
  k_subo_view_instancing = 22,
  k_subo_as = 24,
  k_subo_ms = 25,
  k_subo_depth_stencil2 = 26,
  k_subo_rasterizer1 = 27,
  k_subo_rasterizer2 = 28,
  k_subo_max_valid = 29,
};

struct d3d12_box_t {               // D3D12_BOX (24)
  u32 left, top, front;
  u32 right, bottom, back;
};
static_assert(sizeof(d3d12_box_t) == 24, "D3D12_BOX must be 24 bytes");

struct d3d12_tex_copy_loc_t {      // D3D12_TEXTURE_COPY_LOCATION (48)
  void* p_resource;                // @0
  u32 type;                        // D3D12_TEXTURE_COPY_TYPE @8
  u32 pad;                         // @12
  u8 payload[32];                  // union @16 (PLACED_SUBRESOURCE_FOOTPRINT)
};
static_assert(sizeof(d3d12_tex_copy_loc_t) == 48, "tex copy location 48");

// Forward declare offline COM object types so AddRef/Release/Destroy compose
struct d3d12_blob;
struct d3d12_device;
struct d3d12_queue;
struct d3d12_alloc;
struct d3d12_cmd_list;
struct d3d12_fence;
struct d3d12_desc_heap;
struct d3d12_resource;
struct d3d12_simple_child;

// ---- Shared IUnknown / ID3D12Object base ----------------------------------
struct com_base {
  const void** vtbl;
  std::atomic<u32> refcount{1};
  d3d12_device* device = nullptr;
  void (*destroy_fn)(com_base*) = nullptr;
};

u32 TRT_GUEST_ABI base_addref(void* self) {
  return ++((com_base*)self)->refcount;
}

u32 TRT_GUEST_ABI base_release(void* self) {
  auto* o = (com_base*)self;
  const u32 r = --o->refcount;
  if (r == 0 && o->destroy_fn) o->destroy_fn(o);
  return r;
}

[[maybe_unused]] int TRT_GUEST_ABI base_qi_self(void* self, const void* riid, void** ppv) {
  if (!ppv) return k_e_invalidarg;
  (void)riid;
  *ppv = self;
  base_addref(self);
  return k_s_ok;
}

int TRT_GUEST_ABI child_get_priv(void*, const void*, u32* sz, void*) {
  if (sz) *sz = 0;
  return (int)0x887A0002u;  // DXGI_ERROR_NOT_FOUND
}
int TRT_GUEST_ABI child_set_priv(void*, const void*, u32, const void*) { return k_s_ok; }
int TRT_GUEST_ABI child_set_priv_iface(void*, const void*, void*) { return k_s_ok; }
int TRT_GUEST_ABI child_set_name(void*, const u16*) { return k_s_ok; }

int TRT_GUEST_ABI child_get_device(void* self, const void* riid, void** ppv) {
  if (!ppv) return k_e_invalidarg;
  auto* o = (com_base*)self;
  if (!o->device) { *ppv = nullptr; return k_e_nointerface; }
  (void)riid;
  *ppv = o->device;
  base_addref(o->device);
  return k_s_ok;
}

// ---- ID3DBlob --------------------------------------------------------------
struct d3d12_blob {
  const void** vtbl;
  std::atomic<u32> refcount{1};
  std::vector<u8> bytes;
};

int TRT_GUEST_ABI blob_qi(void* self, const void* riid, void** ppv) {
  if (!ppv) return k_e_invalidarg;
  (void)riid;
  *ppv = self;
  ++((d3d12_blob*)self)->refcount;
  return k_s_ok;
}
u32 TRT_GUEST_ABI blob_addref(void* self) { return ++((d3d12_blob*)self)->refcount; }
u32 TRT_GUEST_ABI blob_release(void* self) {
  auto* b = (d3d12_blob*)self;
  const u32 r = --b->refcount;
  if (r == 0) delete b;
  return r;
}
void* TRT_GUEST_ABI blob_ptr(void* self) {
  auto* b = (d3d12_blob*)self;
  return b->bytes.empty() ? nullptr : b->bytes.data();
}
u64 TRT_GUEST_ABI blob_size(void* self) {
  return ((d3d12_blob*)self)->bytes.size();
}
const void* g_blob_vtbl[5] = {
    (const void*)&blob_qi,
    (const void*)&blob_addref,
    (const void*)&blob_release,
    (const void*)&blob_ptr,
    (const void*)&blob_size,
};

d3d12_blob* make_blob(const void* data, size_t sz) {
  auto* b = new d3d12_blob();
  b->vtbl = g_blob_vtbl;
  if (data && sz) b->bytes.assign((const u8*)data, (const u8*)data + sz);
  return b;
}

// ---- Root-signature serialization / deserialization -------------------------
// Both serializers used to return S_OK with a fixed 64-byte DXBC shell whose
// only payload was the version number: a title could serialize its layout, hand
// the blob to CreateRootSignature, and never learn that nothing had been carried
// (the "recorded" class). This is the real pair:
//   * parse the guest's D3D12_ROOT_SIGNATURE_DESC / ...DESC1 / ...DESC2
//     (root parameters, descriptor tables and ranges, static samplers, flags),
//   * validate it the way the runtime does, and on rejection return an ID3DBlob
//     holding the human-readable reason (exactly what D3D12 does),
//   * emit a self-describing DXBC container,
//   * parse that container back in D3D12Create[Versioned]RootSignatureDeserializer
//     and hand out a real D3D12_ROOT_SIGNATURE_DESC / ...VERSIONED...DESC,
//   * and let ID3D12Device::CreateRootSignature see the actual layout.
//
// Container (little-endian; keeps the DXBC shell so magic sniffers still work):
//   0  u32 'DXBC'        20 u32 container version (1)
//   4  16-byte hash (0)  24 u32 total size
//   28 u32 chunk count (3), then one u32 chunk offset each; every chunk is a
//   4-byte tag followed by its payload:
//     'RTS0' u32 version, u32 flags, u32 num_params, u32 num_samplers
//     'RDAT' u32 num_params, then per parameter 24 bytes
//            {type, visibility, a, b, c, range_count} followed by range_count
//            24-byte ranges {type, num_descriptors, reg, space, flags, offset}
//     'STAT' u32 num_samplers, then num_samplers verbatim 52-byte
//            D3D12_STATIC_SAMPLER_DESC records
// A blob serialized by a real Windows d3d12.dll carries a different RDAT payload;
// it is rejected here with a named reason (never mis-read), while
// CreateRootSignature still accepts it for compatibility and logs that its
// layout is not introspectable.
constexpr u32 k_rsbox_magic = 0x43425844u;  // 'DXBC'
constexpr u32 k_rsbox_format = 1;
constexpr u32 k_rsbox_chunks = 3;
constexpr u32 k_rsbox_header = 32 + 4 * k_rsbox_chunks;  // 44

constexpr u32 k_rsdesc_num_params = 0;
constexpr u32 k_rsdesc_params = 8;
constexpr u32 k_rsdesc_num_samplers = 16;
constexpr u32 k_rsdesc_samplers = 24;
constexpr u32 k_rsdesc_flags = 32;
constexpr u32 k_rsdesc_size = 40;

constexpr u32 k_rsparam_size = 32;
constexpr u32 k_rsparam_union = 8;
constexpr u32 k_rsparam_visibility = 24;

constexpr u32 k_rsrange_size_10 = 20;  // D3D12_DESCRIPTOR_RANGE
constexpr u32 k_rsrange_size_11 = 24;  // D3D12_DESCRIPTOR_RANGE1

constexpr u32 k_rssampler_size = 52;  // D3D12_STATIC_SAMPLER_DESC

// The D3D12 documented limits; exceeding them is what the runtime rejects.
constexpr u32 k_rs_max_params = 64;
constexpr u32 k_rs_max_ranges_per_table = 64;
constexpr u32 k_rs_max_static_samplers = 2032;

struct rs_range {
  u32 type = 0, num = 0, reg = 0, space = 0, flags = 0, offset = 0;
};

struct rs_param {
  u32 type = 0;              // D3D12_ROOT_PARAMETER_TYPE
  u32 visibility = 0;        // D3D12_SHADER_VISIBILITY
  u32 a = 0, b = 0, c = 0;   // constants {reg, space, num32} / descriptor {reg, space, flags}
  std::vector<rs_range> ranges;
};

struct rs_desc {
  u32 version = 1;  // D3D_ROOT_SIGNATURE_VERSION (1 = 1.0, 2 = 1.1, 3 = 1.2)
  u32 flags = 0;
  std::vector<rs_param> params;
  std::vector<u8> samplers;  // N * 52 verbatim records
};

inline u32 rs_get32(const u8* p, size_t off) {
  u32 v = 0;
  memcpy(&v, p + off, 4);
  return v;
}
inline const void* rs_getptr(const u8* p, size_t off) {
  u64 v = 0;
  memcpy(&v, p + off, sizeof(v));
  return (const void*)(uintptr_t)v;
}
inline void rs_put32(u8* p, size_t off, u32 v) { memcpy(p + off, &v, 4); }
inline void rs_putptr(u8* p, size_t off, const void* v) { memcpy(p + off, &v, sizeof(v)); }

// Parse a guest desc body (the D3D12_ROOT_SIGNATURE_DESC at the end of a
// D3D12_VERSIONED_ROOT_SIGNATURE_DESC, or the desc handed to
// D3D12SerializeRootSignature). 1.0/1.1/1.2 differ only in the element sizes.
int rs_parse_desc(const void* body, u32 version, rs_desc* out, char* err, size_t err_sz) {
  if (!out || !err || !err_sz) return k_e_invalidarg;
  if (!body) {
    snprintf(err, err_sz, "pRootSignature is NULL");
    return k_e_invalidarg;
  }
  if (version < 1 || version > 3) {
    snprintf(err, err_sz, "D3D_ROOT_SIGNATURE_VERSION %u is not serializable", version);
    return k_e_invalidarg;
  }
  const u8* b = (const u8*)body;
  out->version = version;
  out->flags = rs_get32(b, k_rsdesc_flags);
  const u32 n = rs_get32(b, k_rsdesc_num_params);
  const void* params = rs_getptr(b, k_rsdesc_params);
  const u32 ns = rs_get32(b, k_rsdesc_num_samplers);
  const void* samplers = rs_getptr(b, k_rsdesc_samplers);
  if (n > k_rs_max_params) {
    snprintf(err, err_sz, "NumParameters is %u, above the D3D12 limit of %u", n,
             k_rs_max_params);
    return k_e_invalidarg;
  }
  if (n && !params) {
    snprintf(err, err_sz, "NumParameters is %u but pParameters is NULL", n);
    return k_e_invalidarg;
  }
  if (ns > k_rs_max_static_samplers) {
    snprintf(err, err_sz, "NumStaticSamplers is %u, above the D3D12 limit of %u", ns,
             k_rs_max_static_samplers);
    return k_e_invalidarg;
  }
  if (ns && !samplers) {
    snprintf(err, err_sz, "NumStaticSamplers is %u but pStaticSamplers is NULL", ns);
    return k_e_invalidarg;
  }

  const u32 range_size = (version >= 2) ? k_rsrange_size_11 : k_rsrange_size_10;
  out->params.assign(n, rs_param());
  for (u32 i = 0; i < n; ++i) {
    const u8* p = (const u8*)params + (size_t)i * k_rsparam_size;
    rs_param& d = out->params[i];
    d.type = rs_get32(p, 0);
    d.visibility = rs_get32(p, k_rsparam_visibility);
    if (d.type > 4) {
      snprintf(err, err_sz, "root parameter %u has an invalid ParameterType %u", i, d.type);
      return k_e_invalidarg;
    }
    if (d.visibility > 7) {
      snprintf(err, err_sz, "root parameter %u has an invalid ShaderVisibility %u", i,
               d.visibility);
      return k_e_invalidarg;
    }
    if (d.type == 0) {  // D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE
      const u32 nr = rs_get32(p, k_rsparam_union);
      const void* ranges = rs_getptr(p, k_rsparam_union + sizeof(void*));
      if (nr > k_rs_max_ranges_per_table) {
        snprintf(err, err_sz,
                 "root parameter %u has %u descriptor ranges, above the D3D12 limit of %u", i,
                 nr, k_rs_max_ranges_per_table);
        return k_e_invalidarg;
      }
      if (nr && !ranges) {
        snprintf(err, err_sz, "root parameter %u has %u ranges but pDescriptorRanges is NULL",
                 i, nr);
        return k_e_invalidarg;
      }
      d.ranges.assign(nr, rs_range());
      for (u32 j = 0; j < nr; ++j) {
        const u8* r = (const u8*)ranges + (size_t)j * range_size;
        rs_range& o = d.ranges[j];
        o.type = rs_get32(r, 0);
        o.num = rs_get32(r, 4);
        o.reg = rs_get32(r, 8);
        o.space = rs_get32(r, 12);
        o.flags = (version >= 2) ? rs_get32(r, 16) : 0;
        o.offset = rs_get32(r, (version >= 2) ? 20 : 16);
        if (o.type > 3) {
          snprintf(err, err_sz, "parameter %u range %u has an invalid RangeType %u", i, j,
                   o.type);
          return k_e_invalidarg;
        }
      }
    } else {
      d.a = rs_get32(p, k_rsparam_union);
      d.b = rs_get32(p, k_rsparam_union + 4);
      d.c = rs_get32(p, k_rsparam_union + 8);
    }
  }

  out->samplers.assign((size_t)ns * k_rssampler_size, 0);
  if (ns) memcpy(out->samplers.data(), samplers, (size_t)ns * k_rssampler_size);
  for (u32 i = 0; i < ns; ++i) {
    const u8* s = out->samplers.data() + (size_t)i * k_rssampler_size;
    const u32 au = rs_get32(s, 4), av = rs_get32(s, 8), aw = rs_get32(s, 12);
    if (au < 1 || au > 4 || av < 1 || av > 4 || aw < 1 || aw > 4) {
      snprintf(err, err_sz,
               "static sampler %u has an invalid AddressU/V/W (%u/%u/%u; valid 1..4)", i, au, av,
               aw);
      return k_e_invalidarg;
    }
    const u32 cmp = rs_get32(s, 24);
    if (cmp < 1 || cmp > 8) {
      snprintf(err, err_sz, "static sampler %u has an invalid ComparisonFunc %u", i, cmp);
      return k_e_invalidarg;
    }
    const u32 border = rs_get32(s, 28);
    if (border > 3) {
      snprintf(err, err_sz, "static sampler %u has an invalid BorderColor %u", i, border);
      return k_e_invalidarg;
    }
    const u32 vis = rs_get32(s, 48);
    if (vis > 7) {
      snprintf(err, err_sz, "static sampler %u has an invalid ShaderVisibility %u", i, vis);
      return k_e_invalidarg;
    }
  }
  return k_s_ok;
}

int rs_emit(const rs_desc& d, void** out_blob) {
  u32 rdat_payload = 4;
  for (const rs_param& p : d.params) {
    rdat_payload += 24;
    if (p.type == 0) rdat_payload += (u32)p.ranges.size() * 24;
  }
  const u32 rts0_payload = 16;
  const u32 stat_payload = 4 + (u32)d.samplers.size();
  const u32 off_rts0 = k_rsbox_header;
  const u32 off_rdat = off_rts0 + 4 + rts0_payload;
  const u32 off_stat = off_rdat + 4 + rdat_payload;
  const u32 total = off_stat + 4 + stat_payload;

  std::vector<u8> buf(total, 0);
  u8* b = buf.data();
  rs_put32(b, 0, k_rsbox_magic);
  rs_put32(b, 20, k_rsbox_format);
  rs_put32(b, 24, total);
  rs_put32(b, 28, k_rsbox_chunks);
  rs_put32(b, 32, off_rts0);
  rs_put32(b, 36, off_rdat);
  rs_put32(b, 40, off_stat);

  rs_put32(b, off_rts0, 0x30535452u);  // 'RTS0'
  rs_put32(b, off_rts0 + 4, d.version);
  rs_put32(b, off_rts0 + 8, d.flags);
  rs_put32(b, off_rts0 + 12, (u32)d.params.size());
  rs_put32(b, off_rts0 + 16, (u32)(d.samplers.size() / k_rssampler_size));

  rs_put32(b, off_rdat, 0x54414452u);  // 'RDAT'
  rs_put32(b, off_rdat + 4, (u32)d.params.size());
  u32 o = off_rdat + 8;
  for (const rs_param& p : d.params) {
    rs_put32(b, o + 0, p.type);
    rs_put32(b, o + 4, p.visibility);
    rs_put32(b, o + 8, p.a);
    rs_put32(b, o + 12, p.b);
    rs_put32(b, o + 16, p.c);
    rs_put32(b, o + 20, (u32)p.ranges.size());
    o += 24;
    for (const rs_range& r : p.ranges) {
      rs_put32(b, o + 0, r.type);
      rs_put32(b, o + 4, r.num);
      rs_put32(b, o + 8, r.reg);
      rs_put32(b, o + 12, r.space);
      rs_put32(b, o + 16, r.flags);
      rs_put32(b, o + 20, r.offset);
      o += 24;
    }
  }

  rs_put32(b, off_stat, 0x54415453u);  // 'STAT'
  rs_put32(b, off_stat + 4, (u32)(d.samplers.size() / k_rssampler_size));
  if (!d.samplers.empty()) memcpy(b + off_stat + 8, d.samplers.data(), d.samplers.size());

  d3d12_blob* blob = make_blob(buf.data(), buf.size());
  if (!blob) return k_e_outofmemory;
  *out_blob = blob;
  return k_s_ok;
}

int rs_read_container(const void* blob, u64 size, rs_desc* out, char* err, size_t err_sz) {
  if (!out || !err || !err_sz) return k_e_invalidarg;
  if (!blob || !size) {
    snprintf(err, err_sz, "blob is NULL or empty");
    return k_e_invalidarg;
  }
  if (size < k_rsbox_header) {
    snprintf(err, err_sz, "blob is %llu bytes, shorter than the 44-byte DXBC header",
             (unsigned long long)size);
    return k_e_invalidarg;
  }
  const u8* b = (const u8*)blob;
  if (rs_get32(b, 0) != k_rsbox_magic) {
    snprintf(err, err_sz, "blob does not begin with the DXBC magic");
    return k_e_invalidarg;
  }
  const u32 total = rs_get32(b, 24);
  const u32 nchunks = rs_get32(b, 28);
  if (total > size || total < k_rsbox_header) {
    snprintf(err, err_sz, "DXBC size field %u disagrees with the %llu-byte blob", total,
             (unsigned long long)size);
    return k_e_invalidarg;
  }
  if (nchunks > 16) {
    snprintf(err, err_sz, "DXBC chunk count %u is implausible", nchunks);
    return k_e_invalidarg;
  }
  const u8* rts0 = nullptr;
  const u8* rdat = nullptr;
  const u8* stat = nullptr;
  u32 rts0_len = 0, rdat_len = 0, stat_len = 0;
  for (u32 i = 0; i < nchunks; ++i) {
    const u32 off = rs_get32(b, 32 + i * 4);
    if (off < k_rsbox_header || (u64)off + 8 > total) continue;
    u32 next = total;
    for (u32 j = 0; j < nchunks; ++j) {
      const u32 other = rs_get32(b, 32 + j * 4);
      if (other > off && other < next) next = other;
    }
    const u32 len = (next > off) ? next - off - 4 : 0;
    const u32 tag = rs_get32(b, off);
    if (tag == 0x30535452u) {
      rts0 = b + off + 4;
      rts0_len = len;
    } else if (tag == 0x54414452u) {
      rdat = b + off + 4;
      rdat_len = len;
    } else if (tag == 0x54415453u) {
      stat = b + off + 4;
      stat_len = len;
    }
  }
  if (!rts0 || rts0_len < 16) {
    snprintf(err, err_sz, "blob has no 'RTS0' chunk (not a Tiramisu-root-signature)");
    return k_e_invalidarg;
  }
  if (!rdat || rdat_len < 4) {
    snprintf(err, err_sz, "blob has no 'RDAT' chunk (parameter data missing)");
    return k_e_invalidarg;
  }

  out->version = rs_get32(rts0, 0);
  out->flags = rs_get32(rts0, 4);
  const u32 n = rs_get32(rts0, 8);
  const u32 ns = rs_get32(rts0, 12);
  if (n > k_rs_max_params || ns > k_rs_max_static_samplers) {
    snprintf(err, err_sz, "'RTS0' declares %u parameters / %u samplers, above the D3D12 limits",
             n, ns);
    return k_e_invalidarg;
  }
  if (rs_get32(rdat, 0) != n) {
    snprintf(err, err_sz, "'RDAT' carries %u parameters but 'RTS0' declares %u",
             rs_get32(rdat, 0), n);
    return k_e_invalidarg;
  }

  out->params.assign(n, rs_param());
  u32 o = 4;
  for (u32 i = 0; i < n; ++i) {
    if (o + 24 > rdat_len) {
      snprintf(err, err_sz, "'RDAT' is truncated at parameter %u", i);
      return k_e_invalidarg;
    }
    rs_param& p = out->params[i];
    p.type = rs_get32(rdat, o);
    p.visibility = rs_get32(rdat, o + 4);
    p.a = rs_get32(rdat, o + 8);
    p.b = rs_get32(rdat, o + 12);
    p.c = rs_get32(rdat, o + 16);
    const u32 nr = rs_get32(rdat, o + 20);
    o += 24;
    if (p.type == 0) {
      if (nr > k_rs_max_ranges_per_table || o + nr * 24 > rdat_len) {
        snprintf(err, err_sz, "'RDAT' has an invalid range count %u at parameter %u", nr, i);
        return k_e_invalidarg;
      }
      p.ranges.assign(nr, rs_range());
      for (u32 j = 0; j < nr; ++j) {
        rs_range& r = p.ranges[j];
        r.type = rs_get32(rdat, o + 0);
        r.num = rs_get32(rdat, o + 4);
        r.reg = rs_get32(rdat, o + 8);
        r.space = rs_get32(rdat, o + 12);
        r.flags = rs_get32(rdat, o + 16);
        r.offset = rs_get32(rdat, o + 20);
        o += 24;
      }
    } else if (nr != 0) {
      snprintf(err, err_sz, "parameter %u is not a descriptor table but carries %u ranges", i,
               nr);
      return k_e_invalidarg;
    }
  }

  if (stat) {
    if (stat_len < 4 || rs_get32(stat, 0) != ns) {
      snprintf(err, err_sz, "'STAT' declares %u samplers but 'RTS0' declares %u",
               stat_len < 4 ? 0 : rs_get32(stat, 0), ns);
      return k_e_invalidarg;
    }
    if ((u64)stat_len < 4 + (u64)ns * k_rssampler_size) {
      snprintf(err, err_sz, "'STAT' is truncated (%u bytes for %u samplers)", stat_len, ns);
      return k_e_invalidarg;
    }
    out->samplers.assign((size_t)ns * k_rssampler_size, 0);
    if (ns) memcpy(out->samplers.data(), stat + 4, (size_t)ns * k_rssampler_size);
  } else if (ns) {
    snprintf(err, err_sz, "'RTS0' declares %u static samplers but there is no 'STAT' chunk", ns);
    return k_e_invalidarg;
  }
  return k_s_ok;
}

// Rebuild the desc structures a deserializer hands to the guest. One buffer per
// (version, versioned) pair so the pointers in it stay valid for the object's
// lifetime, as the real deserializer guarantees.
struct rs_layout {
  std::vector<u8> buf;
};

void rs_build_layout(const rs_desc& d, bool v11, bool versioned, u32 decl_version,
                     rs_layout& out) {
  const u32 n = (u32)d.params.size();
  u32 ranges_total = 0;
  for (const rs_param& p : d.params)
    if (p.type == 0) ranges_total += (u32)p.ranges.size();
  const u32 range_size = v11 ? k_rsrange_size_11 : k_rsrange_size_10;
  const u32 ns = (u32)(d.samplers.size() / k_rssampler_size);
  const u32 desc_size = versioned ? 48 : k_rsdesc_size;  // 8-byte version + body
  const u32 off_params = desc_size;
  const u32 off_ranges = off_params + n * k_rsparam_size;
  const u32 off_samplers = off_ranges + ranges_total * range_size;
  out.buf.assign(off_samplers + (size_t)ns * k_rssampler_size, 0);

  u8* base = out.buf.data();
  const u32 body = versioned ? 8 : 0;
  if (versioned) rs_put32(base, 0, decl_version);
  rs_put32(base, body + k_rsdesc_num_params, n);
  rs_putptr(base, body + k_rsdesc_params, n ? base + off_params : nullptr);
  rs_put32(base, body + k_rsdesc_num_samplers, ns);
  rs_putptr(base, body + k_rsdesc_samplers, ns ? base + off_samplers : nullptr);
  rs_put32(base, body + k_rsdesc_flags, d.flags);

  u32 roff = off_ranges;
  for (u32 i = 0; i < n; ++i) {
    const rs_param& p = d.params[i];
    u8* o = base + off_params + (size_t)i * k_rsparam_size;
    rs_put32(o, 0, p.type);
    rs_put32(o, k_rsparam_visibility, p.visibility);
    if (p.type == 0) {
      const u32 nr = (u32)p.ranges.size();
      rs_put32(o, k_rsparam_union, nr);
      rs_putptr(o, k_rsparam_union + sizeof(void*), nr ? base + roff : nullptr);
      for (u32 j = 0; j < nr; ++j) {
        const rs_range& r = p.ranges[j];
        u8* q = base + roff;
        rs_put32(q, 0, r.type);
        rs_put32(q, 4, r.num);
        rs_put32(q, 8, r.reg);
        rs_put32(q, 12, r.space);
        if (v11) {
          rs_put32(q, 16, r.flags);
          rs_put32(q, 20, r.offset);
        } else {
          rs_put32(q, 16, r.offset);
        }
        roff += range_size;
      }
    } else {
      rs_put32(o, k_rsparam_union, p.a);
      rs_put32(o, k_rsparam_union + 4, p.b);
      if (v11 || p.type == 1) rs_put32(o, k_rsparam_union + 8, p.c);
    }
  }
  if (ns) memcpy(base + off_samplers, d.samplers.data(), d.samplers.size());
}

// ---- ID3D12RootSignatureDeserializer / ID3D12VersionedRootSignatureDeserializer
// QI answers IUnknown plus the exact IID the caller created the object with, so
// the bridge never has to invent (or get wrong) an SDK GUID.
struct d3d12_rs_deser : com_base {
  rs_desc d;
  u8 created_iid[16] = {0};
  bool versioned = false;
  rs_layout plain, v10, v11;
  bool have_plain = false, have_v10 = false, have_v11 = false;
  u32 v11_decl = 2;
};

void rsdeser_destroy(com_base* b) { delete (d3d12_rs_deser*)b; }

int TRT_GUEST_ABI rsdeser_qi(void* self, const void* riid, void** ppv) {
  if (!ppv) return k_e_invalidarg;
  *ppv = nullptr;
  auto* d = (d3d12_rs_deser*)self;
  if (!riid || is_iunknown(riid) || memcmp(riid, d->created_iid, 16) == 0) {
    *ppv = self;
    base_addref(self);
    return k_s_ok;
  }
  const raw_guid* g = (const raw_guid*)riid;
  char line[176];
  snprintf(line, sizeof(line),
           "d3d12: %s QueryInterface({%08x-%04x-%04x-...}) -> E_NOINTERFACE "
           "(only the IID it was created with)",
           d->versioned ? "VersionedRootSignatureDeserializer" : "RootSignatureDeserializer",
           g->d1, g->d2, g->d3);
  trt_core_log(line);
  return k_e_nointerface;
}

const void* TRT_GUEST_ABI rsdeser_get_desc(void* self) {
  auto* d = (d3d12_rs_deser*)self;
  if (!d->have_plain) {
    rs_build_layout(d->d, /*v11=*/false, /*versioned=*/false, 1, d->plain);
    d->have_plain = true;
  }
  return d->plain.buf.data();
}

int TRT_GUEST_ABI vrsdeser_get_at_version(void* self, u32 version, void** ppdesc) {
  if (!ppdesc) return k_e_invalidarg;
  auto* d = (d3d12_rs_deser*)self;
  *ppdesc = nullptr;
  if (version == 1) {
    if (!d->have_v10) {
      rs_build_layout(d->d, /*v11=*/false, /*versioned=*/true, 1, d->v10);
      d->have_v10 = true;
    }
    *ppdesc = d->v10.buf.data();
    return k_s_ok;
  }
  if (version == 2 || version == 3) {
    if (!d->have_v11 || d->v11_decl != version) {
      rs_build_layout(d->d, /*v11=*/true, /*versioned=*/true, version, d->v11);
      d->have_v11 = true;
      d->v11_decl = version;
    }
    *ppdesc = d->v11.buf.data();
    return k_s_ok;
  }
  char line[176];
  snprintf(line, sizeof(line),
           "d3d12: GetRootSignatureDescAtVersion(%u) -> E_INVALIDARG (only 1.0/1.1/1.2 "
           "exist)",
           version);
  trt_core_log(line);
  return k_e_invalidarg;
}

const void* TRT_GUEST_ABI vrsdeser_get_unconverted(void* self) {
  auto* d = (d3d12_rs_deser*)self;
  if (d->d.version >= 2) {
    const u32 want = (d->d.version >= 3) ? 3 : 2;
    if (!d->have_v11 || d->v11_decl != want) {
      rs_build_layout(d->d, /*v11=*/true, /*versioned=*/true, want, d->v11);
      d->have_v11 = true;
      d->v11_decl = want;
    }
    return d->v11.buf.data();
  }
  if (!d->have_v10) {
    rs_build_layout(d->d, /*v11=*/false, /*versioned=*/true, 1, d->v10);
    d->have_v10 = true;
  }
  return d->v10.buf.data();
}

const void* g_rsdeser_vtbl[4] = {
    (const void*)&rsdeser_qi, (const void*)&base_addref, (const void*)&base_release,
    (const void*)&rsdeser_get_desc,
};

const void* g_vrsdeser_vtbl[5] = {
    (const void*)&rsdeser_qi, (const void*)&base_addref, (const void*)&base_release,
    (const void*)&vrsdeser_get_at_version, (const void*)&vrsdeser_get_unconverted,
};

int rs_create_deserializer(const void* src, u64 size, const void* riid, bool versioned,
                           void** out) {
  if (!out) return k_e_invalidarg;
  *out = nullptr;
  char err[224] = {0};
  rs_desc d;
  if (rs_read_container(src, size, &d, err, sizeof(err)) != k_s_ok) {
    char line[352];
    snprintf(line, sizeof(line), "d3d12: %sRootSignatureDeserializer -> E_INVALIDARG (%s)",
             versioned ? "CreateVersioned" : "Create", err);
    trt_core_log(line);
    return k_e_invalidarg;
  }
  auto* o = new d3d12_rs_deser();
  o->d = d;
  o->versioned = versioned;
  o->vtbl = versioned ? g_vrsdeser_vtbl : g_rsdeser_vtbl;
  o->destroy_fn = &rsdeser_destroy;
  if (riid) memcpy(o->created_iid, riid, 16);
  *out = o;
  char line[224];
  snprintf(line, sizeof(line),
           "d3d12: %sRootSignatureDeserializer recovered %u root parameter(s), %u static "
           "sampler(s), version %u",
           versioned ? "CreateVersioned" : "Create", (u32)o->d.params.size(),
           (u32)(o->d.samplers.size() / k_rssampler_size), o->d.version);
  trt_core_log(line);
  return k_s_ok;
}

int rs_serialize(const void* desc, u32 version, bool versioned, void** out_blob,
                 void** out_err) {
  if (out_err) *out_err = nullptr;
  if (out_blob) *out_blob = nullptr;
  if (!out_blob) return k_e_invalidarg;
  char err[224] = {0};
  rs_desc d;
  int hr;
  if (versioned) {
    if (!desc) {
      snprintf(err, sizeof(err), "pRootSignature is NULL");
      hr = k_e_invalidarg;
    } else {
      hr = rs_parse_desc((const u8*)desc + 8, rs_get32((const u8*)desc, 0), &d, err,
                         sizeof(err));
    }
  } else {
    hr = rs_parse_desc(desc, version, &d, err, sizeof(err));
  }
  if (hr != k_s_ok) {
    if (out_err) *out_err = make_blob(err, strlen(err) + 1);
    char line[352];
    snprintf(line, sizeof(line), "d3d12: %s -> E_INVALIDARG (%s)",
             versioned ? "D3D12SerializeVersionedRootSignature"
                       : "D3D12SerializeRootSignature",
             err);
    trt_core_log(line);
    return k_e_invalidarg;
  }
  hr = rs_emit(d, out_blob);
  if (hr != k_s_ok) return hr;
  char line[256];
  snprintf(line, sizeof(line),
           "d3d12: %s serialized %u root parameter(s), %u static sampler(s), version %u, "
           "flags %#x into a %llu-byte DXBC container",
           versioned ? "D3D12SerializeVersionedRootSignature"
                     : "D3D12SerializeRootSignature",
           (u32)d.params.size(), (u32)(d.samplers.size() / k_rssampler_size), d.version,
           d.flags, (unsigned long long)((d3d12_blob*)*out_blob)->bytes.size());
  d3d12_log_once(25, line);
  return k_s_ok;
}

// ---- ID3D12Fence -----------------------------------------------------------
struct fence_waiter {
  u64 target;
  u64 event_handle;
};

struct d3d12_fence : com_base {
  std::mutex mu;
  std::atomic<u64> completed{0};
  u32 flags = 0;  // D3D12_FENCE_FLAGS from CreateFence (Fence1::GetCreationFlags)
  std::vector<fence_waiter> waiters;
};

void fence_destroy(com_base* b) { delete (d3d12_fence*)b; }

void fence_notify(d3d12_fence* f, u64 value) {
  std::vector<u64> ready;
  {
    std::lock_guard<std::mutex> lk(f->mu);
    const u64 prev = f->completed.load();
    if (value > prev) f->completed.store(value);
    const u64 cur = f->completed.load();
    auto it = f->waiters.begin();
    while (it != f->waiters.end()) {
      if (it->target <= cur) {
        if (it->event_handle) ready.push_back(it->event_handle);
        it = f->waiters.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (u64 ev : ready) trt_SetEvent(ev);
}

u64 TRT_GUEST_ABI fence_completed(void* self) {
  return ((d3d12_fence*)self)->completed.load();
}

int TRT_GUEST_ABI fence_set_event(void* self, u64 value, void* hEvent) {
  auto* f = (d3d12_fence*)self;
  const u64 ev = (u64)(uintptr_t)hEvent;
  if (f->completed.load() >= value) {
    if (ev) trt_SetEvent(ev);
    return k_s_ok;
  }
  {
    std::lock_guard<std::mutex> lk(f->mu);
    if (f->completed.load() >= value) {
      if (ev) trt_SetEvent(ev);
      return k_s_ok;
    }
    if (ev) f->waiters.push_back({value, ev});
  }
  return k_s_ok;
}

int TRT_GUEST_ABI fence_signal(void* self, u64 value) {
  fence_notify((d3d12_fence*)self, value);
  return k_s_ok;
}

// ID3D12Fence1::GetCreationFlags — a real getter (flags recorded at create).
u32 TRT_GUEST_ABI fence1_get_creation_flags(void* self) {
  return ((d3d12_fence*)self)->flags;
}

// BUG-6 QI chain: IUnknown + Object + DeviceChild + Pageable + Fence + Fence1
int TRT_GUEST_ABI fence_qi(void* self, const void* riid, void** ppv) {
  static const qi_iid k_fence = TIRAMISU_IID(ID3D12Fence,
      0x0a753dcfu, 0xc4d8, 0x4b91, 0xad, 0xf6, 0xbe, 0x5a, 0x60, 0xd9, 0x5a, 0x76);
  static const qi_iid k_fence1 = TIRAMISU_IID(ID3D12Fence1,
      0x433685feu, 0xe22b, 0x4ca0, 0xa8, 0xdb, 0xb5, 0xb4, 0xf4, 0xdd, 0x0e, 0x4a);
  static const qi_iid* const chain[] = {
      &k_iid_object, &k_iid_device_child, &k_iid_pageable, &k_fence, &k_fence1,
      nullptr};
  return d3d12_qi_chain(self, riid, ppv, chain, "ID3D12Fence");
}

const void* g_fence_vtbl[12] = {
    (const void*)&fence_qi,
    (const void*)&base_addref,
    (const void*)&base_release,
    (const void*)&child_get_priv,
    (const void*)&child_set_priv,
    (const void*)&child_set_priv_iface,
    (const void*)&child_set_name,
    (const void*)&child_get_device,
    (const void*)&fence_completed,          // 8  GetCompletedValue
    (const void*)&fence_set_event,          // 9  SetEventOnCompletion
    (const void*)&fence_signal,             // 10 Signal
    (const void*)&fence1_get_creation_flags,// 11 ID3D12Fence1::GetCreationFlags
};
static_assert(sizeof(g_fence_vtbl) / sizeof(g_fence_vtbl[0]) == 12,
              "ID3D12Fence/Fence1 vtbl must stay 12 slots (Win64 ABI)");

// ---- ID3D12DescriptorHeap --------------------------------------------------
// One parsed descriptor record per heap slot. `kind` is the view type written
// into the slot (0xFF = never written); the rest is the parsed view/sampler
// state so CopyDescriptors can move records between heaps and the recorded
// command-list ops can reference what a root table actually points at.
struct d3d12_view_record {
  u32 kind;               // 0=CBV 1=SRV 2=UAV 3=RTV 4=DSV 5=SAMPLER 0xff=empty
  u32 view_dim;           // ViewDimension (SRV/UAV/RTV/DSV), 0 for CBV/sampler
  u32 format;             // DXGI_FORMAT (0 = derived from the resource)
  u32 flags;              // DSV flags / BUFFER_SRV|UAV flags
  u32 mip;                // MostDetailedMip / MipSlice
  u32 mip_levels;         // MipLevels
  u32 first_slice;        // FirstArraySlice / First2DArrayFace
  u32 num_slices;         // ArraySize / NumCubes
  u32 plane;              // PlaneSlice
  u32 size_bytes;         // CBV SizeInBytes / BUFFER NumElements
  u32 stride;             // BUFFER StructureByteStride
  u32 sampler_filter;     // SAMPLER: D3D12_FILTER
  u32 sampler_cmp;        // SAMPLER: D3D12_COMPARISON_FUNC
  u64 gpu_va;             // CBV BufferLocation / RTAS Location / FirstElement
  void* resource;         // referenced resource (null for CBV/sampler/null-desc)
  void* counter_resource; // UAV counter resource
  u32 addr_u, addr_v, addr_w;  // SAMPLER address modes
  u32 max_anisotropy;          // SAMPLER
  float mip_lod_bias;          // SAMPLER
  float border_color[4];       // SAMPLER
  float min_lod, max_lod;      // SAMPLER
};

constexpr u32 k_desc_kind_empty = 0xffffffffu;
// Hard cap on stored records per heap (a 1M-descriptor heap costs ~72 MB of
// records; beyond the cap handles are bounds-checked but fail VISIBLY).
constexpr u32 k_desc_records_cap = 1u << 20;

struct d3d12_desc_heap : com_base {
  d3d12_heap_desc_info_t desc{};
  u64 cpu_base = 0;
  u64 gpu_base = 0;
  u32 inc = 64;           // per-descriptor handle increment (32 CSU/SAMPLER, 64 RTV/DSV)
  u64 span = 0;           // cpu VA span reserved at create (num * 64)
  std::vector<d3d12_view_record> records;
};

void heap_desc_destroy(com_base* b) {
  auto* h = (d3d12_desc_heap*)b;
  {
    std::lock_guard<std::mutex> lk(g_live_mu);
    g_live_heaps.erase(h);
  }
  delete h;
}

d3d12_heap_desc_info_t* TRT_GUEST_ABI descheap_get_desc(void* self, d3d12_heap_desc_info_t* ret) {
  if (ret) *ret = ((d3d12_desc_heap*)self)->desc;
  return ret;
}

u64* TRT_GUEST_ABI descheap_cpu_start(void* self, u64* ret) {
  if (ret) *ret = ((d3d12_desc_heap*)self)->cpu_base;
  return ret;
}

u64* TRT_GUEST_ABI descheap_gpu_start(void* self, u64* ret) {
  if (ret) *ret = ((d3d12_desc_heap*)self)->gpu_base;
  return ret;
}

// BUG-6 QI chain: IUnknown + Object + DeviceChild + Pageable + DescriptorHeap
int TRT_GUEST_ABI descheap_qi(void* self, const void* riid, void** ppv) {
  static const qi_iid k_descheap = TIRAMISU_IID(ID3D12DescriptorHeap,
      0x8efb471du, 0x616c, 0x4f49, 0x90, 0xf7, 0x12, 0x7b, 0xb7, 0x63, 0xfa, 0x51);
  static const qi_iid* const chain[] = {
      &k_iid_object, &k_iid_device_child, &k_iid_pageable, &k_descheap, nullptr};
  return d3d12_qi_chain(self, riid, ppv, chain, "ID3D12DescriptorHeap");
}

const void* g_descheap_vtbl[11] = {
    (const void*)&descheap_qi,
    (const void*)&base_addref,
    (const void*)&base_release,
    (const void*)&child_get_priv,
    (const void*)&child_set_priv,
    (const void*)&child_set_priv_iface,
    (const void*)&child_set_name,
    (const void*)&child_get_device,
    (const void*)&descheap_get_desc,
    (const void*)&descheap_cpu_start,
    (const void*)&descheap_gpu_start,
};

// ---- ID3D12Resource --------------------------------------------------------
struct d3d12_resource : com_base {
  d3d12_resource_desc_t desc{};
  d3d12_heap_props_t heap_props{};
  u32 heap_flags = 0;
  u64 gpu_va = 0;
  std::vector<u8> backing;
};

void resource_destroy(com_base* b) {
  auto* r = (d3d12_resource*)b;
  {
    std::lock_guard<std::mutex> lk(g_live_mu);
    g_live_resources.erase(r);
  }
  delete r;
}

size_t estimate_res_bytes(const d3d12_resource_desc_t& d) {
  u64 w = d.width ? d.width : 1;
  u64 h = d.height ? d.height : 1;
  u64 bytes = (d.dimension == 1) ? w : (w * h * 4);
  if (bytes > 16u * 1024u * 1024u) bytes = 16u * 1024u * 1024u;
  return (size_t)bytes;
}

int TRT_GUEST_ABI res_map(void* self, u32 subres, const void* read_range, void** ppData) {
  (void)subres; (void)read_range;
  auto* r = (d3d12_resource*)self;
  if (r->backing.empty()) {
    const size_t sz = estimate_res_bytes(r->desc);
    r->backing.resize(sz ? sz : 256);
  }
  if (ppData) *ppData = r->backing.data();
  return k_s_ok;
}

void TRT_GUEST_ABI res_unmap(void*, u32, const void*) {}

d3d12_resource_desc_t* TRT_GUEST_ABI res_get_desc(void* self, d3d12_resource_desc_t* ret) {
  if (ret) *ret = ((d3d12_resource*)self)->desc;
  return ret;
}

u64 TRT_GUEST_ABI res_gpu_va(void* self) {
  return ((d3d12_resource*)self)->gpu_va;
}

// Box-aware subresource copies for WriteToSubresource (slot 12) and
// ReadFromSubresource (slot 13). box == null -> one row of min(pitch, cap)
// bytes at offset 0; box != null -> rows [top,bottom) x slices [front,back),
// row_bytes = right-left, linearized into the backing store as
// (slice * rows + row) * row_bytes. Every mismatch fails VISIBLY with
// E_INVALIDARG and a named log line; an empty box is an S_OK no-op.
int TRT_GUEST_ABI res_write_sub(void* self, u32, const void* box,
                                const void* src, u32 src_row_pitch,
                                u32 src_depth_pitch) {
  auto* r = (d3d12_resource*)self;
  const char* api = "WriteToSubresource";
  if (!src) {
    view_fail(api, "pSrcData is null");
    return k_e_invalidarg;
  }
  if (r->backing.empty()) r->backing.resize(estimate_res_bytes(r->desc));
  const size_t cap = r->backing.size();
  u8* dstp = r->backing.data();
  if (!box) {
    if (src_row_pitch == 0) {
      view_fail(api, "SrcRowPitch is 0");
      return k_e_invalidarg;
    }
    memcpy(dstp, src, src_row_pitch < cap ? src_row_pitch : cap);
  } else {
    const d3d12_box_t* b = (const d3d12_box_t*)box;
    if (b->right < b->left || b->bottom < b->top || b->back < b->front) {
      char reason[128];
      snprintf(reason, sizeof(reason),
               "D3D12_BOX is not monotonic (right %u < left %u, bottom %u < "
               "top %u or back %u < front %u)",
               b->right, b->left, b->bottom, b->top, b->back, b->front);
      view_fail(api, reason);
      return k_e_invalidarg;
    }
    const u64 row_bytes = (u64)(b->right - b->left);
    const u64 rows = (u64)(b->bottom - b->top);
    const u64 slices = (u64)(b->back - b->front);
    if (row_bytes == 0 || rows == 0 || slices == 0) return k_s_ok;  // empty box
    // D3D12_BOX semantics: for a BUFFER, left/right are BYTE offsets in the
    // subresource; for a texture they are texel offsets (the bridge's backing
    // store is 4 bytes per texel, matching estimate_res_bytes).
    const bool is_buffer = (r->desc.dimension == 1);
    const u32 texel = is_buffer ? 1u : 4u;
    const u64 row_bytes_b = row_bytes * texel;
    if (src_row_pitch < row_bytes_b) {
      char reason[128];
      snprintf(reason, sizeof(reason),
               "SrcRowPitch %u is smaller than the box row width %llu",
               src_row_pitch, (unsigned long long)row_bytes_b);
      view_fail(api, reason);
      return k_e_invalidarg;
    }
    if (slices > 1 && (u64)src_depth_pitch < src_row_pitch * rows) {
      char reason[160];
      snprintf(reason, sizeof(reason),
               "SrcDepthPitch %u is smaller than SrcRowPitch %u x %llu rows",
               src_depth_pitch, src_row_pitch, (unsigned long long)rows);
      view_fail(api, reason);
      return k_e_invalidarg;
    }
    // Destination offset of row (z, y) within the backing store: box.left
    // bytes into the row for a buffer, box.left texels for a texture; box
    // top/front select the row/slice the box starts at.
    const u64 row_stride =
        is_buffer ? row_bytes_b : (u64)(r->desc.width ? r->desc.width : 1) * 4u;
    const u64 slice_stride =
        is_buffer ? row_bytes_b * rows
                  : row_stride * (u64)(r->desc.height ? r->desc.height : 1);
    for (u64 z = 0; z < slices; ++z) {
      for (u64 y = 0; y < rows; ++y) {
        const u64 off = (u64)b->left * texel + (u64)b->top * row_stride +
                        (u64)b->front * slice_stride + y * row_stride +
                        z * slice_stride;
        if (off + row_bytes_b > (u64)cap) {
          char reason[160];
          snprintf(reason, sizeof(reason),
                   "box copy of %llu bytes at offset %llu exceeds the "
                   "%zu-byte subresource storage",
                   (unsigned long long)row_bytes_b, (unsigned long long)off,
                   cap);
          view_fail(api, reason);
          return k_e_invalidarg;
        }
        memcpy(dstp + off,
               (const u8*)src + z * (u64)src_depth_pitch + y * (u64)src_row_pitch,
               row_bytes_b);
      }
    }
  }
  d3d12_log_once(27, "d3d12: WriteToSubresource copied into the resource backing store");
  return k_s_ok;
}

int TRT_GUEST_ABI res_read_sub(void* self, void* dst, u32 dst_row_pitch,
                               u32 dst_depth_pitch, u32, const void* box) {
  auto* r = (d3d12_resource*)self;
  const char* api = "ReadFromSubresource";
  if (!dst) {
    view_fail(api, "pDstData is null");
    return k_e_invalidarg;
  }
  if (r->backing.empty()) r->backing.resize(estimate_res_bytes(r->desc));
  const size_t cap = r->backing.size();
  const u8* srcp = r->backing.data();
  if (!box) {
    if (dst_row_pitch == 0) {
      view_fail(api, "DstRowPitch is 0");
      return k_e_invalidarg;
    }
    memcpy(dst, srcp, dst_row_pitch < cap ? dst_row_pitch : cap);
  } else {
    const d3d12_box_t* b = (const d3d12_box_t*)box;
    if (b->right < b->left || b->bottom < b->top || b->back < b->front) {
      char reason[128];
      snprintf(reason, sizeof(reason),
               "D3D12_BOX is not monotonic (right %u < left %u, bottom %u < "
               "top %u or back %u < front %u)",
               b->right, b->left, b->bottom, b->top, b->back, b->front);
      view_fail(api, reason);
      return k_e_invalidarg;
    }
    const u64 row_bytes = (u64)(b->right - b->left);
    const u64 rows = (u64)(b->bottom - b->top);
    const u64 slices = (u64)(b->back - b->front);
    if (row_bytes == 0 || rows == 0 || slices == 0) return k_s_ok;  // empty box
    // D3D12_BOX semantics, mirroring WriteToSubresource: buffer left/right
    // are BYTE offsets, texture left/right are texel offsets (4-byte texels).
    const bool is_buffer = (r->desc.dimension == 1);
    const u32 texel = is_buffer ? 1u : 4u;
    const u64 row_bytes_b = row_bytes * texel;
    if (dst_row_pitch < row_bytes_b) {
      char reason[128];
      snprintf(reason, sizeof(reason),
               "DstRowPitch %u is smaller than the box row width %llu",
               dst_row_pitch, (unsigned long long)row_bytes_b);
      view_fail(api, reason);
      return k_e_invalidarg;
    }
    if (slices > 1 && (u64)dst_depth_pitch < dst_row_pitch * rows) {
      char reason[160];
      snprintf(reason, sizeof(reason),
               "DstDepthPitch %u is smaller than DstRowPitch %u x %llu rows",
               dst_depth_pitch, dst_row_pitch, (unsigned long long)rows);
      view_fail(api, reason);
      return k_e_invalidarg;
    }
    const u64 row_stride =
        is_buffer ? row_bytes_b : (u64)(r->desc.width ? r->desc.width : 1) * 4u;
    const u64 slice_stride =
        is_buffer ? row_bytes_b * rows
                  : row_stride * (u64)(r->desc.height ? r->desc.height : 1);
    for (u64 z = 0; z < slices; ++z) {
      for (u64 y = 0; y < rows; ++y) {
        const u64 off = (u64)b->left * texel + (u64)b->top * row_stride +
                        (u64)b->front * slice_stride + y * row_stride +
                        z * slice_stride;
        if (off + row_bytes_b > (u64)cap) {
          char reason[160];
          snprintf(reason, sizeof(reason),
                   "box copy of %llu bytes at offset %llu exceeds the "
                   "%zu-byte subresource storage",
                   (unsigned long long)row_bytes_b, (unsigned long long)off,
                   cap);
          view_fail(api, reason);
          return k_e_invalidarg;
        }
        memcpy((u8*)dst + z * (u64)dst_depth_pitch + y * (u64)dst_row_pitch,
               srcp + off, row_bytes_b);
      }
    }
  }
  d3d12_log_once(28, "d3d12: ReadFromSubresource copied out of the resource backing store");
  return k_s_ok;
}

int TRT_GUEST_ABI res_get_heap_props(void* self, d3d12_heap_props_t* props, u32* flags) {
  auto* r = (d3d12_resource*)self;
  if (props) *props = r->heap_props;
  if (flags) *flags = r->heap_flags;
  return k_s_ok;
}

// BUG-6 QI chain: IUnknown + Object + DeviceChild + Pageable + Resource.
// ID3D12Resource1/2 add slots beyond this 15-slot vtbl -> E_NOINTERFACE (logged).
int TRT_GUEST_ABI resource_qi(void* self, const void* riid, void** ppv) {
  static const qi_iid k_resource = TIRAMISU_IID(ID3D12Resource,
      0x696442beu, 0xa72e, 0x4059, 0xbc, 0x79, 0x5b, 0x5c, 0x98, 0x04, 0x0f, 0xad);
  static const qi_iid* const chain[] = {
      &k_iid_object, &k_iid_device_child, &k_iid_pageable, &k_resource, nullptr};
  return d3d12_qi_chain(self, riid, ppv, chain, "ID3D12Resource");
}

const void* g_resource_vtbl[15] = {
    (const void*)&resource_qi,
    (const void*)&base_addref,
    (const void*)&base_release,
    (const void*)&child_get_priv,
    (const void*)&child_set_priv,
    (const void*)&child_set_priv_iface,
    (const void*)&child_set_name,
    (const void*)&child_get_device,
    (const void*)&res_map,
    (const void*)&res_unmap,
    (const void*)&res_get_desc,
    (const void*)&res_gpu_va,
    (const void*)&res_write_sub,
    (const void*)&res_read_sub,
    (const void*)&res_get_heap_props,
};
// ABI pins: a reorder here silently breaks every D3D12 game's resource calls.
static_assert(sizeof(g_resource_vtbl) / sizeof(g_resource_vtbl[0]) == 15,
              "ID3D12Resource vtbl must stay 15 slots (Win64 ABI)");

// ---- ID3D12RootSignature / PipelineState / QueryHeap / CommandSignature ----
// One host-side shape for the pure-DeviceChild objects. `is_root_sig` marks
// the objects CreateRootSignature returned (the registry the PSO parsers
// validate pRootSignature against); the PSO fields carry the shader bytecode
// the (graphics/compute/stream) desc parsers extracted.
struct d3d12_simple_child : com_base {
  d3d12_heap_desc_t heap_desc{};
  std::vector<u8> cached_pso;
  // Root-signature layout recovered from the blob CreateRootSignature
  // accepted; 0/0 for a blob this bridge did not serialize.
  u32 rs_num_params = 0;
  u32 rs_num_samplers = 0;
  u32 rs_flags = 0;
  rs_desc rs_layout_store;
  bool is_root_sig = false;
  bool is_compute = false;
  void* root_sig = nullptr;
  void* vs_blob = nullptr;
  u64 vs_size = 0;
  void* ps_blob = nullptr;
  u64 ps_size = 0;
  void* cs_blob = nullptr;
  u64 cs_size = 0;
};

void simple_child_destroy(com_base* b) {
  auto* c = (d3d12_simple_child*)b;
  if (c->is_root_sig) {
    std::lock_guard<std::mutex> lk(g_live_mu);
    g_live_rootsigs.erase(c);
  }
  {
    std::lock_guard<std::mutex> lk(g_live_mu);
    g_live_psos.erase(c);
  }
  delete c;
}

d3d12_heap_desc_t* TRT_GUEST_ABI heap_get_desc(void* self, d3d12_heap_desc_t* ret) {
  if (ret) *ret = ((d3d12_simple_child*)self)->heap_desc;
  return ret;
}

int TRT_GUEST_ABI pso_get_cached_blob(void* self, void** out_blob) {
  if (!out_blob) return k_e_invalidarg;
  auto* p = (d3d12_simple_child*)self;
  const char dummy[16] = {'T','P','S','O', 1, 0, 0, 0};
  *out_blob = p->cached_pso.empty()
                  ? make_blob(dummy, sizeof(dummy))
                  : make_blob(p->cached_pso.data(), p->cached_pso.size());
  return k_s_ok;
}

// BUG-6 QI chain for the pure-DeviceChild shape (RootSignature and
// CommandSignature share this vtbl — both have no methods beyond DeviceChild).
int TRT_GUEST_ABI child8_qi(void* self, const void* riid, void** ppv) {
  static const qi_iid k_rootsig = TIRAMISU_IID(ID3D12RootSignature,
      0xc54a6b66u, 0x72df, 0x4ee8, 0x8b, 0xe5, 0xa9, 0x46, 0xa1, 0x42, 0x92, 0x14);
  static const qi_iid k_cmdsig = TIRAMISU_IID(ID3D12CommandSignature,
      0xc36a797cu, 0xec80, 0x4f0a, 0x89, 0x85, 0xa7, 0xb2, 0x47, 0x50, 0x82, 0xd1);
  static const qi_iid* const chain[] = {
      &k_iid_object, &k_iid_device_child, &k_rootsig, &k_cmdsig, nullptr};
  return d3d12_qi_chain(self, riid, ppv, chain, "ID3D12RootSignature/CommandSignature");
}

const void* g_child8_vtbl[8] = {
    (const void*)&child8_qi, (const void*)&base_addref, (const void*)&base_release,
    (const void*)&child_get_priv, (const void*)&child_set_priv,
    (const void*)&child_set_priv_iface, (const void*)&child_set_name,
    (const void*)&child_get_device,
};

// BUG-6 QI chain: IUnknown + Object + DeviceChild + PipelineState
int TRT_GUEST_ABI pso_qi(void* self, const void* riid, void** ppv) {
  static const qi_iid k_pso = TIRAMISU_IID(ID3D12PipelineState,
      0x765a30f3u, 0xf624, 0x4c6f, 0xa8, 0x28, 0xac, 0xe9, 0x48, 0x62, 0x24, 0x45);
  static const qi_iid* const chain[] = {
      &k_iid_object, &k_iid_device_child, &k_pso, nullptr};
  return d3d12_qi_chain(self, riid, ppv, chain, "ID3D12PipelineState");
}

const void* g_pso_vtbl[9] = {
    (const void*)&pso_qi, (const void*)&base_addref, (const void*)&base_release,
    (const void*)&child_get_priv, (const void*)&child_set_priv,
    (const void*)&child_set_priv_iface, (const void*)&child_set_name,
    (const void*)&child_get_device, (const void*)&pso_get_cached_blob,
};

// BUG-6 QI chain: IUnknown + Object + DeviceChild + QueryHeap
int TRT_GUEST_ABI queryheap_qi(void* self, const void* riid, void** ppv) {
  static const qi_iid k_queryheap = TIRAMISU_IID(ID3D12QueryHeap,
      0x0d9658aeu, 0xed45, 0x469e, 0xa6, 0x1d, 0x97, 0x0e, 0xc5, 0x83, 0xca, 0xb4);
  static const qi_iid* const chain[] = {
      &k_iid_object, &k_iid_device_child, &k_queryheap, nullptr};
  return d3d12_qi_chain(self, riid, ppv, chain, "ID3D12QueryHeap");
}

const void* g_heap_vtbl[9] = {
    (const void*)&queryheap_qi, (const void*)&base_addref, (const void*)&base_release,
    (const void*)&child_get_priv, (const void*)&child_set_priv,
    (const void*)&child_set_priv_iface, (const void*)&child_set_name,
    (const void*)&child_get_device, (const void*)&heap_get_desc,
};

// ---- ID3D12CommandAllocator ------------------------------------------------
struct d3d12_alloc : com_base {
  u32 type = 0;
  std::atomic<u32> reset_count{0};
};
void alloc_destroy(com_base* b) { delete (d3d12_alloc*)b; }
int TRT_GUEST_ABI alloc_reset(void* self) {
  ++((d3d12_alloc*)self)->reset_count;
  return k_s_ok;
}
// BUG-6 QI chain: IUnknown + Object + DeviceChild + CommandAllocator
int TRT_GUEST_ABI alloc_qi(void* self, const void* riid, void** ppv) {
  static const qi_iid k_alloc = TIRAMISU_IID(ID3D12CommandAllocator,
      0x6102dee4u, 0xaf59, 0x4b09, 0xb9, 0x99, 0xb4, 0x4d, 0x73, 0xf0, 0x9b, 0x24);
  static const qi_iid* const chain[] = {
      &k_iid_object, &k_iid_device_child, &k_alloc, nullptr};
  return d3d12_qi_chain(self, riid, ppv, chain, "ID3D12CommandAllocator");
}

const void* g_alloc_vtbl[9] = {
    (const void*)&alloc_qi, (const void*)&base_addref, (const void*)&base_release,
    (const void*)&child_get_priv, (const void*)&child_set_priv,
    (const void*)&child_set_priv_iface, (const void*)&child_set_name,
    (const void*)&child_get_device, (const void*)&alloc_reset,
};

// ---- ID3D12GraphicsCommandList ---------------------------------------------
struct d3d12_vb_view_t {
  u64 buffer_location;
  u32 size_in_bytes;
  u32 stride_in_bytes;
};
struct d3d12_ib_view_t {
  u64 buffer_location;
  u32 size_in_bytes;
  u32 format;
};
struct d3d12_viewport_t {
  float top_left_x;
  float top_left_y;
  float width;
  float height;
  float min_depth;
  float max_depth;
};
struct d3d12_rect_t {
  i32 left;
  i32 top;
  i32 right;
  i32 bottom;
};

struct d3d12_cmd_list : com_base {
  u32 type = 0;
  bool closed = false;
  u32 draws = 0;
  struct draw_rec { u32 verts, insts, sv, siv; };
  std::vector<draw_rec> recorded_draws;  // submission replay payload
  u32 dispatches = 0;
  u32 clears = 0;
  u32 barriers = 0;
  u32 copies = 0;
  u32 resolves = 0;
  // SetDescriptorHeaps: the heaps currently bound to the list (replaced on
  // every call, like the real API).
  std::vector<void*> bound_heaps;
  // Root-slot bindings recorded per call (slot, GPU handle/VA, graphics?).
  struct root_table {
    u32 slot;
    u64 gpu;
    bool graphics;
  };
  std::vector<root_table> root_tables;
  struct root_cbv {
    u32 slot;
    u64 va;
    bool graphics;
  };
  std::vector<root_cbv> root_cbvs;
  // Pipeline, IA, RS, OM, Query, and Marker state recorded per call.
  u32 topology = 0;
  u32 viewport_count = 0;
  d3d12_viewport_t viewport0{};
  u32 scissor_count = 0;
  d3d12_rect_t scissor0{};
  float blend_factor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  u32 stencil_ref = 0;
  void* bound_pso = nullptr;
  void* gfx_rootsig = nullptr;
  void* comp_rootsig = nullptr;
  u32 root_consts = 0;
  u32 root_srv_uav_binds = 0;
  u32 vb_binds = 0;
  d3d12_vb_view_t vb0{};
  bool ib_bound = false;
  d3d12_ib_view_t ib{};
  u32 rtv_count = 0;
  bool dsv_bound = false;
  u32 bundles_executed = 0;
  u32 discards = 0;
  u32 queries = 0;
  u32 markers = 0;
  i32 event_depth = 0;
  u64 predication_va = 0;
};
void cmdlist_destroy(com_base* b) {
  {
    std::lock_guard<std::mutex> lk(g_live_mu);
    g_live_cmdlists.erase(b);
  }
  delete (d3d12_cmd_list*)b;
}

u32 TRT_GUEST_ABI cmd_get_type(void* self) { return ((d3d12_cmd_list*)self)->type; }
int TRT_GUEST_ABI cmd_close(void* self) {
  auto* cl = (d3d12_cmd_list*)self;
  if (cl->closed) return k_e_invalidarg;
  cl->closed = true;
  return k_s_ok;
}
static void cmdlist_clear_recorded_state(d3d12_cmd_list* cl, void* pso) {
  cl->draws = cl->dispatches = cl->clears = cl->barriers = 0;
  cl->copies = cl->resolves = 0;
  cl->bound_heaps.clear();
  cl->root_tables.clear();
  cl->root_cbvs.clear();
  cl->topology = 0;
  cl->viewport_count = 0;
  cl->viewport0 = {};
  cl->scissor_count = 0;
  cl->scissor0 = {};
  cl->blend_factor[0] = cl->blend_factor[1] = cl->blend_factor[2] = cl->blend_factor[3] = 1.0f;
  cl->stencil_ref = 0;
  cl->bound_pso = pso;
  cl->gfx_rootsig = nullptr;
  cl->comp_rootsig = nullptr;
  cl->root_consts = 0;
  cl->root_srv_uav_binds = 0;
  cl->vb_binds = 0;
  cl->vb0 = {};
  cl->ib_bound = false;
  cl->ib = {};
  cl->rtv_count = 0;
  cl->dsv_bound = false;
  cl->bundles_executed = 0;
  cl->discards = 0;
  cl->queries = 0;
  cl->markers = 0;
  cl->event_depth = 0;
  cl->predication_va = 0;
}
int TRT_GUEST_ABI cmd_reset(void* self, void* alloc, void* pso) {
  if (!alloc) return k_e_invalidarg;
  auto* cl = (d3d12_cmd_list*)self;
  cl->closed = false;
  cmdlist_clear_recorded_state(cl, pso);
  return k_s_ok;
}
void TRT_GUEST_ABI cmd_clear_state(void* self, void* pso) {
  cmdlist_clear_recorded_state((d3d12_cmd_list*)self, pso);
}
void TRT_GUEST_ABI cmd_draw_inst(void* self, u32 verts, u32 insts, u32 sv,
                                 u32 siv) {
  auto* cl = (d3d12_cmd_list*)self;
  ++cl->draws;
  cl->recorded_draws.push_back({verts, insts, sv, siv});
  // fail-visible: the draw is RECORDED into the list (offline never renders);
  // the TRIANGLE GATE asserts this line exists in the host log.
  char line[96];
  snprintf(line, sizeof(line),
           "d3d12: cl DrawInstanced(%u,%u,%u,%u) recorded (draws=%u)",
           verts, insts, sv, siv, cl->draws);
  trt_core_log(line);
}
void TRT_GUEST_ABI cmd_draw_idx(void* self, u32, u32, u32, i32, u32) { ++((d3d12_cmd_list*)self)->draws; }
void TRT_GUEST_ABI cmd_dispatch(void* self, u32, u32, u32) { ++((d3d12_cmd_list*)self)->dispatches; }
void TRT_GUEST_ABI cmd_barrier(void* self, u32 n, const void*) { ((d3d12_cmd_list*)self)->barriers += n; }
void TRT_GUEST_ABI cmd_clear_rtv(void* self, u64, const void*, u32, const void*) { ++((d3d12_cmd_list*)self)->clears; }
void TRT_GUEST_ABI cmd_clear_dsv(void* self, u64, u32, float, u8, u32, const void*) { ++((d3d12_cmd_list*)self)->clears; }
void TRT_GUEST_ABI cmd_ia_set_topo(void* self, u32 topo) {
  ((d3d12_cmd_list*)self)->topology = topo;
}
void TRT_GUEST_ABI cmd_rs_set_viewports(void* self, u32 n, const d3d12_viewport_t* vps) {
  auto* cl = (d3d12_cmd_list*)self;
  cl->viewport_count = n;
  if (n && vps) cl->viewport0 = vps[0];
}
void TRT_GUEST_ABI cmd_rs_set_scissors(void* self, u32 n, const d3d12_rect_t* rects) {
  auto* cl = (d3d12_cmd_list*)self;
  cl->scissor_count = n;
  if (n && rects) cl->scissor0 = rects[0];
}
void TRT_GUEST_ABI cmd_om_set_blend_factor(void* self, const float* bf) {
  auto* cl = (d3d12_cmd_list*)self;
  if (bf) {
    for (int i = 0; i < 4; ++i) cl->blend_factor[i] = bf[i];
  } else {
    for (int i = 0; i < 4; ++i) cl->blend_factor[i] = 1.0f;
  }
}
void TRT_GUEST_ABI cmd_om_set_stencil_ref(void* self, u32 ref) {
  ((d3d12_cmd_list*)self)->stencil_ref = ref;
}
void TRT_GUEST_ABI cmd_set_pso(void* self, void* pso) {
  auto* cl = (d3d12_cmd_list*)self;
  if (pso && !is_live_pso(pso)) {
    view_fail("cl SetPipelineState", "pPipelineState was not created by this device");
    return;
  }
  cl->bound_pso = pso;
}
void TRT_GUEST_ABI cmd_exec_bundle(void* self, void* bundle) {
  auto* cl = (d3d12_cmd_list*)self;
  if (!bundle || !is_live_cmdlist(bundle)) {
    view_fail("cl ExecuteBundle",
              !bundle ? "pCommandList is null" : "bundle was not created by this device");
    return;
  }
  ++cl->bundles_executed;
}
void TRT_GUEST_ABI cmd_set_comp_rootsig(void* self, void* rs) {
  auto* cl = (d3d12_cmd_list*)self;
  if (rs && !is_live_root_sig(rs)) {
    view_fail("cl SetComputeRootSignature", "pRootSignature was not created by this device");
    return;
  }
  cl->comp_rootsig = rs;
}
void TRT_GUEST_ABI cmd_set_gfx_rootsig(void* self, void* rs) {
  auto* cl = (d3d12_cmd_list*)self;
  if (rs && !is_live_root_sig(rs)) {
    view_fail("cl SetGraphicsRootSignature", "pRootSignature was not created by this device");
    return;
  }
  cl->gfx_rootsig = rs;
}
void TRT_GUEST_ABI cmd_set_comp_root_const(void* self, u32, u32, u32) {
  ++((d3d12_cmd_list*)self)->root_consts;
}
void TRT_GUEST_ABI cmd_set_gfx_root_const(void* self, u32, u32, u32) {
  ++((d3d12_cmd_list*)self)->root_consts;
}
void TRT_GUEST_ABI cmd_set_comp_root_consts(void* self, u32, u32 n, const void*, u32) {
  ((d3d12_cmd_list*)self)->root_consts += n;
}
void TRT_GUEST_ABI cmd_set_gfx_root_consts(void* self, u32, u32 n, const void*, u32) {
  ((d3d12_cmd_list*)self)->root_consts += n;
}
void TRT_GUEST_ABI cmd_set_comp_root_srv(void* self, u32, u64) {
  ++((d3d12_cmd_list*)self)->root_srv_uav_binds;
}
void TRT_GUEST_ABI cmd_set_gfx_root_srv(void* self, u32, u64) {
  ++((d3d12_cmd_list*)self)->root_srv_uav_binds;
}
void TRT_GUEST_ABI cmd_set_comp_root_uav(void* self, u32, u64) {
  ++((d3d12_cmd_list*)self)->root_srv_uav_binds;
}
void TRT_GUEST_ABI cmd_set_gfx_root_uav(void* self, u32, u64) {
  ++((d3d12_cmd_list*)self)->root_srv_uav_binds;
}
void TRT_GUEST_ABI cmd_ia_set_ib(void* self, const d3d12_ib_view_t* view) {
  auto* cl = (d3d12_cmd_list*)self;
  if (view) {
    cl->ib_bound = true;
    cl->ib = *view;
  } else {
    cl->ib_bound = false;
    cl->ib = {};
  }
}
void TRT_GUEST_ABI cmd_ia_set_vbs(void* self, u32, u32 n, const d3d12_vb_view_t* views) {
  auto* cl = (d3d12_cmd_list*)self;
  cl->vb_binds += n;
  if (n && views) cl->vb0 = views[0];
}
void TRT_GUEST_ABI cmd_so_set_targets(void* self, u32, u32, const void*) {
  (void)self;
}
void TRT_GUEST_ABI cmd_om_set_rtvs(void* self, u32 n, const void*, int, const void* dsv) {
  auto* cl = (d3d12_cmd_list*)self;
  cl->rtv_count = n;
  cl->dsv_bound = (dsv != nullptr);
}
void TRT_GUEST_ABI cmd_clear_uav_uint(void* self, u64, u64, void*, const u32*, u32, const void*) {
  ++((d3d12_cmd_list*)self)->clears;
}
void TRT_GUEST_ABI cmd_clear_uav_float(void* self, u64, u64, void*, const float*, u32, const void*) {
  ++((d3d12_cmd_list*)self)->clears;
}
void TRT_GUEST_ABI cmd_discard_resource(void* self, void* res, const void*) {
  auto* cl = (d3d12_cmd_list*)self;
  if (!res || !is_live_resource(res)) {
    view_fail("cl DiscardResource",
              !res ? "pResource is null" : "resource was not created by this device");
    return;
  }
  ++cl->discards;
}
void TRT_GUEST_ABI cmd_begin_query(void* self, void*, u32, u32) {
  ++((d3d12_cmd_list*)self)->queries;
}
void TRT_GUEST_ABI cmd_end_query(void* self, void*, u32, u32) {
  ++((d3d12_cmd_list*)self)->queries;
}
void TRT_GUEST_ABI cmd_resolve_query(void* self, void*, u32, u32, u32, void* dst, u64) {
  auto* cl = (d3d12_cmd_list*)self;
  if (dst && !is_live_resource(dst)) {
    view_fail("cl ResolveQueryData", "pDestinationBuffer was not created by this device");
    return;
  }
  ++cl->queries;
}
void TRT_GUEST_ABI cmd_set_predication(void* self, void* buf, u64 off, u32) {
  auto* cl = (d3d12_cmd_list*)self;
  if (buf && !is_live_resource(buf)) {
    view_fail("cl SetPredication", "pBuffer was not created by this device");
    return;
  }
  cl->predication_va = buf ? (off + 1ULL) : 0ULL;
}
void TRT_GUEST_ABI cmd_set_marker(void* self, u32, const void*, u32) {
  ++((d3d12_cmd_list*)self)->markers;
}
void TRT_GUEST_ABI cmd_begin_event(void* self, u32, const void*, u32) {
  auto* cl = (d3d12_cmd_list*)self;
  ++cl->markers;
  ++cl->event_depth;
}
void TRT_GUEST_ABI cmd_end_event(void* self) {
  auto* cl = (d3d12_cmd_list*)self;
  if (cl->event_depth > 0) --cl->event_depth;
}

// ---- command-list copies + descriptor/root slots (0-based vtbl slots) ------
// Every op validates its arguments against the live-object registries and
// either records real state or fails VISIBLY (E_INVALIDARG + named log line);
// successes log once so per-frame calls cannot flood the log.

void TRT_GUEST_ABI cmd_copy_buffer_region(void* self, void* dst, u64 dst_off,
                                          void* src, u64 src_off, u64 num) {
  auto* cl = (d3d12_cmd_list*)self;
  if (!dst || !src) {
    view_fail("cl CopyBufferRegion",
              !dst ? "pDstBuffer is null" : "pSrcBuffer is null");
    return;
  }
  if (!is_live_resource(dst) || !is_live_resource(src)) {
    char reason[128];
    snprintf(reason, sizeof(reason),
             "resource 0x%llx was not created by this device",
             (unsigned long long)(uintptr_t)(!is_live_resource(dst) ? dst : src));
    view_fail("cl CopyBufferRegion", reason);
    return;
  }
  ++cl->copies;
  char line[160];
  snprintf(line, sizeof(line),
           "d3d12: cl CopyBufferRegion recorded (dst 0x%llx+%llu <- src "
           "0x%llx+%llu, %llu bytes)",
           (unsigned long long)(uintptr_t)dst, (unsigned long long)dst_off,
           (unsigned long long)(uintptr_t)src, (unsigned long long)src_off,
           (unsigned long long)num);
  d3d12_log_once(0, line);
}

void TRT_GUEST_ABI cmd_copy_texture_region(void* self,
                                           const d3d12_tex_copy_loc_t* dst,
                                           u32 dst_x, u32 dst_y, u32 dst_z,
                                           const d3d12_tex_copy_loc_t* src,
                                           const d3d12_box_t* src_box) {
  auto* cl = (d3d12_cmd_list*)self;
  if (!dst || !src) {
    view_fail("cl CopyTextureRegion",
              !dst ? "pDst is null" : "pSrc is null");
    return;
  }
  if (!dst->p_resource || !src->p_resource) {
    view_fail("cl CopyTextureRegion",
              !dst->p_resource ? "pDst->pResource is null"
                               : "pSrc->pResource is null");
    return;
  }
  if (!is_live_resource(dst->p_resource) || !is_live_resource(src->p_resource)) {
    char reason[128];
    snprintf(reason, sizeof(reason),
             "resource 0x%llx was not created by this device",
             (unsigned long long)(uintptr_t)(!is_live_resource(dst->p_resource)
                                                 ? dst->p_resource
                                                 : src->p_resource));
    view_fail("cl CopyTextureRegion", reason);
    return;
  }
  if (dst->type > 1 || src->type > 1) {
    char reason[96];
    snprintf(reason, sizeof(reason),
             "TextureCopyType %u/%u outside 0..1 (SUBRESOURCE, "
             "PLACED_FOOTPRINT)",
             dst->type, src->type);
    view_fail("cl CopyTextureRegion", reason);
    return;
  }
  if (src_box &&
      (src_box->right < src_box->left || src_box->bottom < src_box->top ||
       src_box->back < src_box->front)) {
    view_fail("cl CopyTextureRegion",
              "D3D12_BOX is not monotonic (right<left, bottom<top or "
              "back<front)");
    return;
  }
  ++cl->copies;
  char line[160];
  snprintf(line, sizeof(line),
           "d3d12: cl CopyTextureRegion recorded (dst 0x%llx @%u,%u,%u <- src "
           "0x%llx box %s)",
           (unsigned long long)(uintptr_t)dst->p_resource, dst_x, dst_y, dst_z,
           (unsigned long long)(uintptr_t)src->p_resource,
           src_box ? "set" : "null");
  d3d12_log_once(1, line);
}

void TRT_GUEST_ABI cmd_copy_resource(void* self, void* dst, void* src) {
  auto* cl = (d3d12_cmd_list*)self;
  if (!dst || !src) {
    view_fail("cl CopyResource", !dst ? "pDstResource is null"
                                      : "pSrcResource is null");
    return;
  }
  if (!is_live_resource(dst) || !is_live_resource(src)) {
    char reason[128];
    snprintf(reason, sizeof(reason),
             "resource 0x%llx was not created by this device",
             (unsigned long long)(uintptr_t)(!is_live_resource(dst) ? dst : src));
    view_fail("cl CopyResource", reason);
    return;
  }
  ++cl->copies;
  char line[128];
  snprintf(line, sizeof(line),
           "d3d12: cl CopyResource recorded (dst 0x%llx <- src 0x%llx)",
           (unsigned long long)(uintptr_t)dst,
           (unsigned long long)(uintptr_t)src);
  d3d12_log_once(2, line);
}

void TRT_GUEST_ABI cmd_copy_tiles(void* self, void* tiled,
                                  const void* start_coord,
                                  const void* region_size, void* buffer,
                                  u64 buf_off, u32 flags) {
  auto* cl = (d3d12_cmd_list*)self;
  if (!tiled || !buffer) {
    view_fail("cl CopyTiles",
              !tiled ? "pTiledResource is null" : "pBuffer is null");
    return;
  }
  if (!is_live_resource(tiled) || !is_live_resource(buffer)) {
    char reason[128];
    snprintf(reason, sizeof(reason),
             "resource 0x%llx was not created by this device",
             (unsigned long long)(uintptr_t)(!is_live_resource(tiled) ? tiled
                                                                      : buffer));
    view_fail("cl CopyTiles", reason);
    return;
  }
  ++cl->copies;
  char line[160];
  snprintf(line, sizeof(line),
           "d3d12: cl CopyTiles recorded (tiled 0x%llx <- buffer 0x%llx+%llu "
           "flags 0x%x)",
           (unsigned long long)(uintptr_t)tiled,
           (unsigned long long)(uintptr_t)buffer, (unsigned long long)buf_off,
           flags);
  d3d12_log_once(3, line);
}

void TRT_GUEST_ABI cmd_resolve_subresource(void* self, void* dst, u32 dst_sub,
                                           void* src, u32 src_sub, u32 format) {
  auto* cl = (d3d12_cmd_list*)self;
  if (!dst || !src) {
    view_fail("cl ResolveSubresource", !dst ? "pDstResource is null"
                                            : "pSrcResource is null");
    return;
  }
  if (!is_live_resource(dst) || !is_live_resource(src)) {
    char reason[128];
    snprintf(reason, sizeof(reason),
             "resource 0x%llx was not created by this device",
             (unsigned long long)(uintptr_t)(!is_live_resource(dst) ? dst : src));
    view_fail("cl ResolveSubresource", reason);
    return;
  }
  if (format == 0) {
    view_fail("cl ResolveSubresource",
              "Format is DXGI_FORMAT_UNKNOWN (resolve needs a typed format)");
    return;
  }
  ++cl->resolves;
  char line[128];
  snprintf(line, sizeof(line),
           "d3d12: cl ResolveSubresource recorded (dst 0x%llx sub %u <- src "
           "0x%llx sub %u format %u)",
           (unsigned long long)(uintptr_t)dst, dst_sub,
           (unsigned long long)(uintptr_t)src, src_sub, format);
  d3d12_log_once(4, line);
}

void TRT_GUEST_ABI cmd_set_desc_heaps(void* self, u32 num,
                                      void* const* heaps) {
  auto* cl = (d3d12_cmd_list*)self;
  if (num == 0 || !heaps) {
    view_fail("cl SetDescriptorHeaps",
              num == 0 ? "NumDescriptorHeaps is 0"
                       : "ppDescriptorHeaps is null");
    return;
  }
  for (u32 i = 0; i < num; ++i) {
    if (!heaps[i]) {
      char reason[96];
      snprintf(reason, sizeof(reason), "ppDescriptorHeaps[%u] is null", i);
      view_fail("cl SetDescriptorHeaps", reason);
      return;
    }
    bool live = false;
    {
      std::lock_guard<std::mutex> lk(g_live_mu);
      live = g_live_heaps.count(heaps[i]) != 0;
    }
    if (!live) {
      char reason[128];
      snprintf(reason, sizeof(reason),
               "descriptor heap 0x%llx was not created by this device",
               (unsigned long long)(uintptr_t)heaps[i]);
      view_fail("cl SetDescriptorHeaps", reason);
      return;
    }
  }
  cl->bound_heaps.assign(heaps, heaps + num);
  char line[128];
  snprintf(line, sizeof(line),
           "d3d12: cl SetDescriptorHeaps recorded (%u heap%s bound)",
           num, num == 1 ? "" : "s");
  d3d12_log_once(5, line);
}

// Root-descriptor-table / root-CBV bindings. The GPU handle (or VA) is
// soft-resolved against the live heaps: an unresolvable handle is a WARN-ONCE
// (the real runtime only faults at draw time), not a hard failure.
static void cmd_bind_root_table(d3d12_cmd_list* cl, u32 slot, u64 gpu,
                                bool graphics) {
  cl->root_tables.push_back({slot, gpu, graphics});
  char reason[192];
  u32 idx = 0;
  if (!resolve_gpu_handle(gpu, &idx, "RootDescriptorTable", reason,
                          sizeof(reason))) {
    char line[288];
    snprintf(line, sizeof(line),
             "d3d12: cl %s RootDescriptorTable GPU handle 0x%llx does not "
             "resolve to a live heap slot (%s) - accepted, recorded as-is",
             graphics ? "Graphics" : "Compute", (unsigned long long)gpu, reason);
    d3d12_log_once(12, line);
  }
  char line[128];
  snprintf(line, sizeof(line),
           "d3d12: cl %s RootDescriptorTable recorded (slot %u gpu 0x%llx)",
           graphics ? "Graphics" : "Compute", slot, (unsigned long long)gpu);
  d3d12_log_once(6, line);
}

static void cmd_bind_root_cbv(d3d12_cmd_list* cl, u32 slot, u64 va,
                              bool graphics) {
  cl->root_cbvs.push_back({slot, va, graphics});
  if (va == 0) {
    d3d12_log_once(13,
                   "d3d12: cl RootConstantBufferView GPU VA is 0 - accepted, "
                   "recorded as-is");
  }
  char line[128];
  snprintf(line, sizeof(line),
           "d3d12: cl %s RootConstantBufferView recorded (slot %u va 0x%llx)",
           graphics ? "Graphics" : "Compute", slot, (unsigned long long)va);
  d3d12_log_once(7, line);
}

void TRT_GUEST_ABI cmd_set_compute_root_table(void* self, u32 slot, u64 gpu) {
  cmd_bind_root_table((d3d12_cmd_list*)self, slot, gpu, false);
}

void TRT_GUEST_ABI cmd_set_graphics_root_table(void* self, u32 slot, u64 gpu) {
  cmd_bind_root_table((d3d12_cmd_list*)self, slot, gpu, true);
}

void TRT_GUEST_ABI cmd_set_compute_root_cbv(void* self, u32 slot, u64 va) {
  cmd_bind_root_cbv((d3d12_cmd_list*)self, slot, va, false);
}

void TRT_GUEST_ABI cmd_set_graphics_root_cbv(void* self, u32 slot, u64 va) {
  cmd_bind_root_cbv((d3d12_cmd_list*)self, slot, va, true);
}

// ---- ID3D12GraphicsCommandList1..9 versioned slots (60-83) ----------------
// ABI-correct arities verified against the xwin SDK d3d12.h vtbl declarations.
void TRT_GUEST_ABI gcl_atomic_copy_uint(void*, void*, u32, u64, const void*, u32, u32, u32) { gcl_stub_log(0, "AtomicCopyBufferUINT"); }
void TRT_GUEST_ABI gcl_atomic_copy_uint64(void*, void*, u32, u64, const void*, u32, u32, u32) { gcl_stub_log(1, "AtomicCopyBufferUINT64"); }
void TRT_GUEST_ABI gcl_om_set_depth_bounds(void*, float, float, float) { gcl_stub_log(2, "OMSetDepthBounds"); }
void TRT_GUEST_ABI gcl_set_sample_positions(void*, u32, u32, const void*, const void*) { gcl_stub_log(3, "SetSamplePositions"); }
void TRT_GUEST_ABI gcl_resolve_subresource_region(void*, void*, u32, u32, void*, u32, u32, u32, u32, u32) { gcl_stub_log(4, "ResolveSubresourceRegion"); }
void TRT_GUEST_ABI gcl_set_view_instance_mask(void*, u32, u32) { gcl_stub_log(5, "SetViewInstanceMask"); }
void TRT_GUEST_ABI gcl_write_buffer_immediate(void*, u32, const void*, u32, u32) { gcl_stub_log(6, "WriteBufferImmediate"); }
void TRT_GUEST_ABI gcl_set_protected_resource_session(void*, void*) { gcl_stub_log(7, "SetProtectedResourceSession"); }
void TRT_GUEST_ABI gcl_begin_render_pass(void*, u32, const void*, const void*, const void*, u32) { gcl_stub_log(8, "BeginRenderPass"); }
void TRT_GUEST_ABI gcl_end_render_pass(void*) { gcl_stub_log(9, "EndRenderPass"); }
void TRT_GUEST_ABI gcl_initialize_meta_command(void*, void*, const void*, const void*, u32) { gcl_stub_log(10, "InitializeMetaCommand"); }
void TRT_GUEST_ABI gcl_execute_meta_command(void*, void*, const void*, const void*, u32) { gcl_stub_log(11, "ExecuteMetaCommand"); }
void TRT_GUEST_ABI gcl_build_rtas(void*, const void*, u32, const void*) { gcl_stub_log(12, "BuildRaytracingAccelerationStructure"); }
void TRT_GUEST_ABI gcl_emit_rtas_postbuild(void*, const void*, u32, void*) { gcl_stub_log(13, "EmitRaytracingAccelerationStructurePostbuildInfo"); }
void TRT_GUEST_ABI gcl_copy_rtas(void*, void*, void*, void*) { gcl_stub_log(14, "CopyRaytracingAccelerationStructure"); }
void TRT_GUEST_ABI gcl_set_pipeline_state1(void*, void*) { gcl_stub_log(15, "SetPipelineState1"); }
void TRT_GUEST_ABI gcl_dispatch_rays(void*, const void*, const void*, const void*) { gcl_stub_log(16, "DispatchRays"); }
void TRT_GUEST_ABI gcl_rs_set_shading_rate(void*, u32, u32) { gcl_stub_log(17, "RSSetShadingRate"); }
void TRT_GUEST_ABI gcl_rs_set_shading_rate_image(void*, void*) { gcl_stub_log(18, "RSSetShadingRateImage"); }
void TRT_GUEST_ABI gcl_dispatch_mesh(void*, u32, u32, const void*) { gcl_stub_log(19, "DispatchMesh"); }
void TRT_GUEST_ABI gcl_barrier(void*, u32, const void*) { gcl_stub_log(20, "Barrier"); }
void TRT_GUEST_ABI gcl_om_set_front_back_stencil_ref(void*, u32, u32, u32) { gcl_stub_log(21, "OMSetFrontAndBackStencilRef"); }
void TRT_GUEST_ABI gcl_rs_set_depth_bias(void*, float, float, u32, u32) { gcl_stub_log(22, "RSSetDepthBias"); }
void TRT_GUEST_ABI gcl_ia_set_index_buffer_strip_cut(void*, u32) { gcl_stub_log(23, "IASetIndexBufferStripCutValue"); }

// BUG-6 QI chain: IUnknown + Object + DeviceChild + CommandList +
// GraphicsCommandList base + versions 1-9 (vtbl covers all 84 slots).
int TRT_GUEST_ABI cmdlist_qi(void* self, const void* riid, void** ppv) {
  static const qi_iid k_cmdlist = TIRAMISU_IID(ID3D12CommandList,
      0x7116d91cu, 0xe7e4, 0x47ce, 0xb8, 0xc6, 0xec, 0x81, 0x68, 0xf4, 0x37, 0xe5);
  static const qi_iid k_gcl = TIRAMISU_IID(ID3D12GraphicsCommandList,
      0x5b160d0fu, 0xac1b, 0x4185, 0x8b, 0xa8, 0xb3, 0xae, 0x42, 0xa5, 0xa4, 0x55);
  static const qi_iid k_gcl1 = TIRAMISU_IID(ID3D12GraphicsCommandList1,
      0x553103fbu, 0x1fe7, 0x4557, 0xbb, 0x38, 0x94, 0x6d, 0x7d, 0x0e, 0x7c, 0xa7);
  static const qi_iid k_gcl2 = TIRAMISU_IID(ID3D12GraphicsCommandList2,
      0x38c3e585u, 0xff17, 0x412c, 0x91, 0x50, 0x4f, 0xc6, 0xf9, 0xd7, 0x2a, 0x28);
  static const qi_iid k_gcl3 = TIRAMISU_IID(ID3D12GraphicsCommandList3,
      0x6fda83a7u, 0xb84c, 0x4e38, 0x9a, 0xc8, 0xc7, 0xbd, 0x22, 0x01, 0x6b, 0x3d);
  static const qi_iid k_gcl4 = TIRAMISU_IID(ID3D12GraphicsCommandList4,
      0x8754318eu, 0xd3a9, 0x4541, 0x98, 0xcf, 0x64, 0x5b, 0x50, 0xdc, 0x48, 0x74);
  static const qi_iid k_gcl5 = TIRAMISU_IID(ID3D12GraphicsCommandList5,
      0x55050859u, 0x4024, 0x474c, 0x87, 0xf5, 0x64, 0x72, 0xea, 0xee, 0x44, 0xea);
  static const qi_iid k_gcl6 = TIRAMISU_IID(ID3D12GraphicsCommandList6,
      0xc3827890u, 0xe548, 0x4cfa, 0x96, 0xcf, 0x56, 0x89, 0xa9, 0x37, 0x0f, 0x80);
  static const qi_iid k_gcl7 = TIRAMISU_IID(ID3D12GraphicsCommandList7,
      0xdd171223u, 0x8b61, 0x4769, 0x90, 0xe3, 0x16, 0x0c, 0xcd, 0xe4, 0xe2, 0xc1);
  static const qi_iid k_gcl8 = TIRAMISU_IID(ID3D12GraphicsCommandList8,
      0xee936ef9u, 0x599d, 0x4d28, 0x93, 0x8e, 0x23, 0xc4, 0xad, 0x05, 0xce, 0x51);
  static const qi_iid k_gcl9 = TIRAMISU_IID(ID3D12GraphicsCommandList9,
      0x34ed2808u, 0xffe6, 0x4c2b, 0xb1, 0x1a, 0xca, 0xbd, 0x2b, 0x0c, 0x59, 0xe1);
  static const qi_iid* const chain[] = {
      &k_iid_object, &k_iid_device_child, &k_cmdlist, &k_gcl,
      &k_gcl1, &k_gcl2, &k_gcl3, &k_gcl4, &k_gcl5, &k_gcl6, &k_gcl7, &k_gcl8,
      &k_gcl9, nullptr};
  return d3d12_qi_chain(self, riid, ppv, chain, "ID3D12GraphicsCommandList");
}

const void* g_cmdlist_vtbl[84] = {
    (const void*)&cmdlist_qi,          // 0 QueryInterface (BUG-6 version chain)
    (const void*)&base_addref,         // 1 AddRef
    (const void*)&base_release,        // 2 Release
    (const void*)&child_get_priv,      // 3 GetPrivateData
    (const void*)&child_set_priv,      // 4 SetPrivateData
    (const void*)&child_set_priv_iface,// 5 SetPrivateDataInterface
    (const void*)&child_set_name,      // 6 SetName
    (const void*)&child_get_device,    // 7 GetDevice
    (const void*)&cmd_get_type,        // 8 GetType
    (const void*)&cmd_close,           // 9 Close
    (const void*)&cmd_reset,           // 10 Reset
    (const void*)&cmd_clear_state,     // 11 ClearState
    (const void*)&cmd_draw_inst,       // 12 DrawInstanced
    (const void*)&cmd_draw_idx,        // 13 DrawIndexedInstanced
    (const void*)&cmd_dispatch,        // 14 Dispatch
    (const void*)&cmd_copy_buffer_region,  // 15 CopyBufferRegion
    (const void*)&cmd_copy_texture_region, // 16 CopyTextureRegion
    (const void*)&cmd_copy_resource,       // 17 CopyResource
    (const void*)&cmd_copy_tiles,          // 18 CopyTiles
    (const void*)&cmd_resolve_subresource, // 19 ResolveSubresource
    (const void*)&cmd_ia_set_topo,            // 20 IASetPrimitiveTopology
    (const void*)&cmd_rs_set_viewports,       // 21 RSSetViewports
    (const void*)&cmd_rs_set_scissors,        // 22 RSSetScissorRects
    (const void*)&cmd_om_set_blend_factor,    // 23 OMSetBlendFactor
    (const void*)&cmd_om_set_stencil_ref,     // 24 OMSetStencilRef
    (const void*)&cmd_set_pso,                // 25 SetPipelineState
    (const void*)&cmd_barrier,                // 26 ResourceBarrier
    (const void*)&cmd_exec_bundle,            // 27 ExecuteBundle
    (const void*)&cmd_set_desc_heaps,         // 28 SetDescriptorHeaps
    (const void*)&cmd_set_comp_rootsig,       // 29 SetComputeRootSignature
    (const void*)&cmd_set_gfx_rootsig,        // 30 SetGraphicsRootSignature
    (const void*)&cmd_set_compute_root_table,   // 31 SetComputeRootDescriptorTable
    (const void*)&cmd_set_graphics_root_table,  // 32 SetGraphicsRootDescriptorTable
    (const void*)&cmd_set_comp_root_const,    // 33 SetComputeRoot32BitConstant
    (const void*)&cmd_set_gfx_root_const,     // 34 SetGraphicsRoot32BitConstant
    (const void*)&cmd_set_comp_root_consts,   // 35 SetComputeRoot32BitConstants
    (const void*)&cmd_set_gfx_root_consts,    // 36 SetGraphicsRoot32BitConstants
    (const void*)&cmd_set_compute_root_cbv,     // 37 SetComputeRootConstantBufferView
    (const void*)&cmd_set_graphics_root_cbv,    // 38 SetGraphicsRootConstantBufferView
    (const void*)&cmd_set_comp_root_srv,      // 39 SetComputeRootShaderResourceView
    (const void*)&cmd_set_gfx_root_srv,       // 40 SetGraphicsRootShaderResourceView
    (const void*)&cmd_set_comp_root_uav,      // 41 SetComputeRootUnorderedAccessView
    (const void*)&cmd_set_gfx_root_uav,       // 42 SetGraphicsRootUnorderedAccessView
    (const void*)&cmd_ia_set_ib,              // 43 IASetIndexBuffer
    (const void*)&cmd_ia_set_vbs,             // 44 IASetVertexBuffers
    (const void*)&cmd_so_set_targets,         // 45 SOSetTargets
    (const void*)&cmd_om_set_rtvs,            // 46 OMSetRenderTargets
    (const void*)&cmd_clear_dsv,              // 47 ClearDepthStencilView
    (const void*)&cmd_clear_rtv,              // 48 ClearRenderTargetView
    (const void*)&cmd_clear_uav_uint,         // 49 ClearUnorderedAccessViewUint
    (const void*)&cmd_clear_uav_float,        // 50 ClearUnorderedAccessViewFloat
    (const void*)&cmd_discard_resource,       // 51 DiscardResource
    (const void*)&cmd_begin_query,            // 52 BeginQuery
    (const void*)&cmd_end_query,              // 53 EndQuery
    (const void*)&cmd_resolve_query,          // 54 ResolveQueryData
    (const void*)&cmd_set_predication,        // 55 SetPredication
    (const void*)&cmd_set_marker,             // 56 SetMarker
    (const void*)&cmd_begin_event,            // 57 BeginEvent
    (const void*)&cmd_end_event,              // 58 EndEvent
    (const void*)&cmd_draw_inst,       // 59 ExecuteIndirect
    // ---- ID3D12GraphicsCommandList1..9 extensions (slots 60-83) ------------
    // Logged no-ops: the slot EXISTS (ABI-correct arity) so versioned QI can
    // succeed without the game calling off the end of the vtbl; the render
    // behavior is a sprint-2 item 27/34 deliverable. First call per method
    // logs loudly (gcl_stub_log), later calls are silent.
    (const void*)&gcl_atomic_copy_uint,     // 60 GCL1 AtomicCopyBufferUINT
    (const void*)&gcl_atomic_copy_uint64,   // 61 GCL1 AtomicCopyBufferUINT64
    (const void*)&gcl_om_set_depth_bounds,  // 62 GCL1 OMSetDepthBounds
    (const void*)&gcl_set_sample_positions, // 63 GCL1 SetSamplePositions
    (const void*)&gcl_resolve_subresource_region, // 64 GCL1 ResolveSubresourceRegion
    (const void*)&gcl_set_view_instance_mask,     // 65 GCL1 SetViewInstanceMask
    (const void*)&gcl_write_buffer_immediate,     // 66 GCL2 WriteBufferImmediate
    (const void*)&gcl_set_protected_resource_session, // 67 GCL3 SetProtectedResourceSession
    (const void*)&gcl_begin_render_pass,    // 68 GCL4 BeginRenderPass
    (const void*)&gcl_end_render_pass,      // 69 GCL4 EndRenderPass
    (const void*)&gcl_initialize_meta_command,    // 70 GCL4 InitializeMetaCommand
    (const void*)&gcl_execute_meta_command,       // 71 GCL4 ExecuteMetaCommand
    (const void*)&gcl_build_rtas,           // 72 GCL4 BuildRaytracingAccelerationStructure
    (const void*)&gcl_emit_rtas_postbuild,  // 73 GCL4 EmitRaytracingAccelerationStructurePostbuildInfo
    (const void*)&gcl_copy_rtas,            // 74 GCL4 CopyRaytracingAccelerationStructure
    (const void*)&gcl_set_pipeline_state1,  // 75 GCL4 SetPipelineState1
    (const void*)&gcl_dispatch_rays,        // 76 GCL4 DispatchRays
    (const void*)&gcl_rs_set_shading_rate,  // 77 GCL5 RSSetShadingRate
    (const void*)&gcl_rs_set_shading_rate_image,  // 78 GCL5 RSSetShadingRateImage
    (const void*)&gcl_dispatch_mesh,        // 79 GCL6 DispatchMesh
    (const void*)&gcl_barrier,              // 80 GCL7 Barrier
    (const void*)&gcl_om_set_front_back_stencil_ref, // 81 GCL8 OMSetFrontAndBackStencilRef
    (const void*)&gcl_rs_set_depth_bias,    // 82 GCL9 RSSetDepthBias
    (const void*)&gcl_ia_set_index_buffer_strip_cut, // 83 GCL9 IASetIndexBufferStripCutValue
};
static_assert(sizeof(g_cmdlist_vtbl) / sizeof(g_cmdlist_vtbl[0]) == 84,
              "ID3D12GraphicsCommandList1..9 vtbl must stay 84 slots (Win64 ABI)");

// ---- ID3D12CommandQueue ----------------------------------------------------
// Real ABI: ID3D12CommandQueue has exactly 13 vtable slots (0-12).
//   0-2  IUnknown (QueryInterface, AddRef, Release)
//   3-6  ID3D12Object (GetPrivateData, SetPrivateData, SetPrivateDataInterface,
//        SetName)
//   7    ID3D12Pageable::GetDevice
//   8    ExecuteCommandLists
//   9    Signal
//   10   Wait
//   11   GetTimestampFrequency
//   12   GetClockCalibration
// The vtbl below must stay in exactly this order; d3d12_vtbl_selfcheck()
// enforces it at registration time.
struct d3d12_submission {
  u32 topology = 0;
  std::vector<d3d12_cmd_list::draw_rec> draws;
};

struct d3d12_queue : com_base {
  d3d12_queue_desc_t desc{};
  std::atomic<u64> executed_lists{0};
  // TRIANGLE GATE support: ExecuteCommandLists records the submitted command
  // lists here so offline tests can assert the game's submission landed.
  std::mutex mu;
  std::vector<void*> recorded_lists;
  std::vector<d3d12_submission> submissions;  // replay payload per ECL list
};

void queue_destroy(com_base* b) {
  {
    std::lock_guard<std::mutex> lk(g_track_mu);
    g_queues.erase(b);
  }
  delete (d3d12_queue*)b;
}

// BUG-6 QI chain: IUnknown + Object + DeviceChild + Pageable + CommandQueue
int TRT_GUEST_ABI queue_qi(void* self, const void* riid, void** ppv) {
  static const qi_iid k_queue = TIRAMISU_IID(ID3D12CommandQueue,
      0x0ec870a6u, 0x5d7e, 0x4c22, 0x8c, 0xfc, 0x5b, 0xaa, 0xe0, 0x76, 0x16, 0xed);
  static const qi_iid* const chain[] = {
      &k_iid_object, &k_iid_device_child, &k_iid_pageable, &k_queue, nullptr};
  return d3d12_qi_chain(self, riid, ppv, chain, "ID3D12CommandQueue");
}

[[maybe_unused]] void TRT_GUEST_ABI queue_update_tile(void*, void*, u32, const void*, const void*, void*, u32, const void*, const void*, const void*, u32) {}
[[maybe_unused]] void TRT_GUEST_ABI queue_copy_tile(void*, void*, const void*, const void*, void*, u64, u32) {}

void TRT_GUEST_ABI queue_exec_lists(void* self, u32 count, void* const* lists) {
  auto* q = (d3d12_queue*)self;
  if (!count || !lists) return;
  q->executed_lists += count;
  {
    std::lock_guard<std::mutex> lk(q->mu);
    for (u32 i = 0; i < count; ++i) {
      if (!lists[i]) continue;
      q->recorded_lists.push_back(lists[i]);
      // Snapshot the list's recorded draws as a D3D11-equivalent submission.
      d3d12_submission sub;
      auto* cl = (d3d12_cmd_list*)lists[i];
      sub.topology = cl->topology;
      sub.draws = cl->recorded_draws;
      q->submissions.push_back(std::move(sub));
    }
  }
  // fail-visible: submission landed; the TRIANGLE GATE asserts this line.
  char line[80];
  snprintf(line, sizeof(line),
           "d3d12: queue ExecuteCommandLists(%u) recorded (total=%llu)", count,
           (unsigned long long)q->executed_lists.load());
  trt_core_log(line);

  // Hand the replay payload to the host GPU path when one is installed.
  const trt_d3d12_hooks* hooks = trt_d3d12_hooks_get();
  if (hooks && hooks->submit) {
    std::vector<trt_d3d12_draw_desc> draws;
    {
      std::lock_guard<std::mutex> lk2(q->mu);
      for (const auto& sub : q->submissions) {
        for (const auto& dr : sub.draws) {
          draws.push_back({sub.topology, dr.verts, dr.insts, dr.sv, dr.siv});
        }
      }
    }
    trt_d3d12_submit_desc sd{(u32)draws.size(), draws.data()};
    const int hr = hooks->submit(&sd);
    snprintf(line, sizeof(line),
             "d3d12: host submit of %u draw(s) hr=0x%08x", sd.count,
             (unsigned)hr);
    trt_core_log(line);
  } else {
    trt_core_log("d3d12: no host submit hook — submission recorded only");
  }
}

[[maybe_unused]] void TRT_GUEST_ABI queue_marker_evt(void*, u32, const void*, u32) {}
[[maybe_unused]] void TRT_GUEST_ABI queue_end_evt(void*) {}

int TRT_GUEST_ABI queue_signal(void*, void* fence, u64 value) {
  if (!fence) return k_e_invalidarg;
  fence_notify((d3d12_fence*)fence, value);
  return k_s_ok;
}

int TRT_GUEST_ABI queue_wait(void*, void* fence, u64 value) {
  if (!fence) return k_e_invalidarg;
  (void)value;
  return k_s_ok;
}

int TRT_GUEST_ABI queue_ts_freq(void*, u64* freq) {
  if (!freq) return k_e_invalidarg;
  *freq = 100000000ULL;
  return k_s_ok;
}

int TRT_GUEST_ABI queue_clock_cal(void*, u64* gpu_ts, u64* cpu_ts) {
  if (gpu_ts) *gpu_ts = 1000;
  if (cpu_ts) *cpu_ts = 1000;
  return k_s_ok;
}

[[maybe_unused]] d3d12_queue_desc_t* TRT_GUEST_ABI queue_get_desc(void* self, d3d12_queue_desc_t* ret) {
  if (ret) *ret = ((d3d12_queue*)self)->desc;
  return ret;
}

// Exact ID3D12CommandQueue ABI: 13 slots, ExecuteCommandLists at slot 8.
// NOTE: queue_update_tile / queue_copy_tile / queue_marker_evt / queue_end_evt
// / queue_get_desc remain defined above but are intentionally NOT registered
// in the vtbl — ID3D12CommandQueue has no such methods, and padding the table
// was what pushed Signal/Wait/ExecuteCommandLists onto the wrong slots
// (BUG-1: every game's render submission silently no-oped).
const void* g_queue_vtbl[13] = {
    (const void*)&queue_qi,             // 0  IUnknown::QueryInterface
    (const void*)&base_addref,          // 1  IUnknown::AddRef
    (const void*)&base_release,         // 2  IUnknown::Release
    (const void*)&child_get_priv,       // 3  ID3D12Object::GetPrivateData
    (const void*)&child_set_priv,       // 4  ID3D12Object::SetPrivateData
    (const void*)&child_set_priv_iface, // 5  ID3D12Object::SetPrivateDataInterface
    (const void*)&child_set_name,       // 6  ID3D12Object::SetName
    (const void*)&child_get_device,     // 7  ID3D12Pageable::GetDevice
    (const void*)&queue_exec_lists,     // 8  ExecuteCommandLists  <- critical
    (const void*)&queue_signal,         // 9  Signal
    (const void*)&queue_wait,           // 10 Wait
    (const void*)&queue_ts_freq,        // 11 GetTimestampFrequency
    (const void*)&queue_clock_cal,      // 12 GetClockCalibration
};

static_assert(sizeof(g_queue_vtbl) / sizeof(g_queue_vtbl[0]) == 13,
              "ID3D12CommandQueue ABI: exactly 13 vtable slots (0-12)");

// Runtime ABI guard for the TRIANGLE GATE: verifies the queue vtbl layout
// slot-by-slot (the function identities that games depend on). Returns false
// (and logs the first bad slot) if any slot is wrong; called from
// trt_bridge_register_d3d12 so a reorder cannot regress silently.
int d3d12_vtbl_selfcheck(void) {
  const void* want[13] = {
      (const void*)&queue_qi,
      (const void*)&base_addref,
      (const void*)&base_release,
      (const void*)&child_get_priv,
      (const void*)&child_set_priv,
      (const void*)&child_set_priv_iface,
      (const void*)&child_set_name,
      (const void*)&child_get_device,
      (const void*)&queue_exec_lists,
      (const void*)&queue_signal,
      (const void*)&queue_wait,
      (const void*)&queue_ts_freq,
      (const void*)&queue_clock_cal,
  };
  for (int i = 0; i < 13; ++i) {
    if (g_queue_vtbl[i] != want[i]) {
      char line[96];
      snprintf(line, sizeof(line), "d3d12: vtbl selfcheck FAILED slot %d", i);
      trt_core_log(line);
      return 0;
    }
  }
  // Structural sanity: the critical slot must hold ExecuteCommandLists.
  if (g_queue_vtbl[8] != (const void*)&queue_exec_lists) {
    trt_core_log("d3d12: vtbl selfcheck FAILED slot 8 (ExecuteCommandLists)");
    return 0;
  }
  return 1;
}

// ---- ID3D12Device4 ---------------------------------------------------------
struct d3d12_device : com_base {
  u32 max_fl = k_fl_12_1;
  d3d12_luid_t luid{0x1337u, 0};
};

void device_destroy(com_base* b) { delete (d3d12_device*)b; }

int TRT_GUEST_ABI dev_qi(void* self, const void* riid, void** ppv) {
  if (!ppv) return k_e_invalidarg;
  if (!riid || is_iunknown(riid) ||
      guid_eq(riid, 0x189819f1u, 0x1db6, 0x4b57, 0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7) || // ID3D12Device
      guid_eq(riid, 0x77acce80u, 0x638e, 0x4e65, 0x88, 0x95, 0xc1, 0xf2, 0x33, 0x86, 0x86, 0x3e) || // ID3D12Device1
      guid_eq(riid, 0x30baa41eu, 0xb15b, 0x475c, 0xa0, 0xbb, 0x1a, 0xf5, 0xc5, 0xb6, 0x43, 0x28) || // ID3D12Device2
      guid_eq(riid, 0x81dadc15u, 0x2bad, 0x4392, 0x93, 0xc5, 0x10, 0x13, 0x45, 0xc4, 0xaa, 0x98) || // ID3D12Device3
      guid_eq(riid, 0xe865df17u, 0xa9ee, 0x46f9, 0xa4, 0x63, 0x30, 0x98, 0x31, 0x5a, 0xa2, 0xe5) || // ID3D12Device4
      guid_eq(riid, 0x8b4f173bu, 0x2fea, 0x4b80, 0x8f, 0x58, 0x43, 0x07, 0x19, 0x1a, 0xb9, 0x5d) || // ID3D12Device5
      guid_eq(riid, 0xc70b221bu, 0x40e4, 0x4a17, 0x89, 0xaf, 0x02, 0x5a, 0x07, 0x27, 0xa6, 0xdc) || // ID3D12Device6
      guid_eq(riid, 0x5c014b53u, 0x68a1, 0x4b9b, 0x8b, 0xd1, 0xdd, 0x60, 0x46, 0xb9, 0x35, 0x8b) || // ID3D12Device7
      guid_eq(riid, 0x9218e6bbu, 0xf944, 0x4f7e, 0xa7, 0x5c, 0xb1, 0xb2, 0xc7, 0xb7, 0x01, 0xf3) || // ID3D12Device8
      guid_eq(riid, 0x4c80e962u, 0xf032, 0x4f60, 0xbc, 0x9e, 0xeb, 0xc2, 0xcf, 0xa1, 0xd8, 0x3c) || // ID3D12Device9
      guid_eq(riid, 0xc4fec28fu, 0x7966, 0x4e95, 0x9f, 0x94, 0xf4, 0x31, 0xcb, 0x56, 0xc3, 0xb8)) { // ID3D12Object (xwin SDK d3d12.h)
    *ppv = self;
    base_addref(self);
    return k_s_ok;
  }
  *ppv = nullptr;
  return k_e_nointerface;
}

u32 TRT_GUEST_ABI dev_node_count(void*) { return 1; }

int TRT_GUEST_ABI dev_create_queue(void* self, const d3d12_queue_desc_t* desc,
                                   const void*, void** out_q) {
  if (!desc || !out_q) return k_e_invalidarg;
  auto* q = new d3d12_queue();
  q->vtbl = g_queue_vtbl;
  q->device = (d3d12_device*)self;
  q->destroy_fn = &queue_destroy;
  q->desc = *desc;
  {
    std::lock_guard<std::mutex> lk(g_track_mu);
    g_queues.insert(q);
  }
  *out_q = q;
  return k_s_ok;
}

int TRT_GUEST_ABI dev_create_alloc(void* self, u32 type, const void*, void** out_alloc) {
  if (!out_alloc) return k_e_invalidarg;
  auto* a = new d3d12_alloc();
  a->vtbl = g_alloc_vtbl;
  a->device = (d3d12_device*)self;
  a->destroy_fn = &alloc_destroy;
  a->type = type;
  *out_alloc = a;
  return k_s_ok;
}

// ---- PSO creation (device slots 10 / 11 / 47) ------------------------------
// Real parsing + validation of the pipeline-state descriptions. Validation is
// deliberately LENIENT where a stale guest mirror reads zeros at the real
// offsets (guest_triangle.c passes a 600-byte mirror of the 656-byte
// D3D12_GRAPHICS_PIPELINE_STATE_DESC, so PrimitiveTopologyType and
// NumRenderTargets read as 0 here and cached-blob/flags read past the mirror
// — those are warn-once/accepted, never hard failures).

// Root-signature liveness: null is accepted (real D3D12 derives one from the
// shaders); a non-null pointer that is not a CreateRootSignature product
// fails VISIBLY.
static bool root_sig_ok(void* rs, char* reason, size_t reason_sz) {
  if (!rs || is_live_root_sig(rs)) return true;
  snprintf(reason, reason_sz,
           "pRootSignature 0x%llx was not returned by CreateRootSignature",
           (unsigned long long)(uintptr_t)rs);
  return false;
}

// Shader bytecode pointer/length must be paired (both null or both set).
static bool shader_pair_ok(const d3d12_shader_bytecode_t& s, const char* name,
                           char* reason, size_t reason_sz) {
  if ((s.p_shader_bytecode != nullptr) == (s.bytecode_length != 0)) return true;
  snprintf(reason, reason_sz,
           "%s bytecode pointer/length are not paired (ptr 0x%llx len %llu)",
           name, (unsigned long long)(uintptr_t)s.p_shader_bytecode,
           (unsigned long long)s.bytecode_length);
  return false;
}

// Warn-once when a shader blob does not start with the DXBC container magic.
static void warn_dxbc(const d3d12_shader_bytecode_t& s, const char* name) {
  if (s.p_shader_bytecode && s.bytecode_length >= 4 &&
      memcmp(s.p_shader_bytecode, "DXBC", 4) != 0) {
    char line[128];
    snprintf(line, sizeof(line),
             "d3d12: %s bytecode does not start with 'DXBC' (accepted, "
             "recorded as-is)",
             name);
    d3d12_log_once(8, line);
  }
}

static int pso_finish(void* self, d3d12_simple_child* p, void** out_pso,
                      const char* api, u32 ok_bit) {
  p->vtbl = g_pso_vtbl;
  p->device = (d3d12_device*)self;
  p->destroy_fn = &simple_child_destroy;
  {
    std::lock_guard<std::mutex> lk(g_live_mu);
    g_live_psos.insert(p);
  }
  *out_pso = p;
  char line[96];
  snprintf(line, sizeof(line), "d3d12: %s recorded", api);
  d3d12_log_once(ok_bit, line);
  return k_s_ok;
}

int TRT_GUEST_ABI dev_create_pso_gfx(void* self, const d3d12_gpsd_t* desc,
                                     const void*, void** out_pso) {
  char reason[192];
  if (!out_pso) {
    view_fail("CreateGraphicsPipelineState", "ppPipelineState is null");
    return k_e_invalidarg;
  }
  if (!desc) {
    view_fail("CreateGraphicsPipelineState", "pPipelineStateDesc is null");
    return k_e_invalidarg;
  }
  if (!root_sig_ok(desc->p_root_signature, reason, sizeof(reason))) {
    view_fail("CreateGraphicsPipelineState", reason);
    return k_e_invalidarg;
  }
  if (!desc->vs.p_shader_bytecode && !desc->vs.bytecode_length) {
    view_fail("CreateGraphicsPipelineState",
              "VS bytecode is null (a graphics PSO needs a vertex shader)");
    return k_e_invalidarg;
  }
  const d3d12_shader_bytecode_t* shaders[5] = {
      &desc->vs, &desc->ps, &desc->ds, &desc->hs, &desc->gs};
  const char* names[5] = {"VS", "PS", "DS", "HS", "GS"};
  for (int i = 0; i < 5; ++i) {
    if (!shader_pair_ok(*shaders[i], names[i], reason, sizeof(reason))) {
      view_fail("CreateGraphicsPipelineState", reason);
      return k_e_invalidarg;
    }
    warn_dxbc(*shaders[i], names[i]);
  }
  if (desc->input_element_count > 0 && !desc->p_input_elements) {
    char r2[128];
    snprintf(r2, sizeof(r2),
             "InputLayout NumElements %u with a null pInputElementDescs",
             desc->input_element_count);
    view_fail("CreateGraphicsPipelineState", r2);
    return k_e_invalidarg;
  }
  if (desc->num_render_targets > 8) {
    char r2[96];
    snprintf(r2, sizeof(r2), "NumRenderTargets %u exceeds 8",
             desc->num_render_targets);
    view_fail("CreateGraphicsPipelineState", r2);
    return k_e_invalidarg;
  }
  if (desc->primitive_topology_type > 4) {
    char r2[96];
    snprintf(r2, sizeof(r2),
             "PrimitiveTopologyType %u outside 0..4",
             desc->primitive_topology_type);
    view_fail("CreateGraphicsPipelineState", r2);
    return k_e_invalidarg;
  }
  if (desc->primitive_topology_type == 0) {
    // A stale 600-byte guest mirror reads 0 at the real @572 offset.
    d3d12_log_once(
        10,
        "d3d12: CreateGraphicsPipelineState PrimitiveTopologyType is 0 "
        "(accepted; a stale guest PSO mirror reads zeros at the real "
        "offsets)");
  }
  auto* p = new d3d12_simple_child();
  p->root_sig = desc->p_root_signature;
  p->vs_blob = const_cast<void*>(desc->vs.p_shader_bytecode);
  p->vs_size = desc->vs.bytecode_length;
  p->ps_blob = const_cast<void*>(desc->ps.p_shader_bytecode);
  p->ps_size = desc->ps.bytecode_length;
  return pso_finish(self, p, out_pso, "CreateGraphicsPipelineState", 24);
}

int TRT_GUEST_ABI dev_create_pso_compute(void* self, const d3d12_cpsd_t* desc,
                                         const void*, void** out_pso) {
  char reason[192];
  if (!out_pso) {
    view_fail("CreateComputePipelineState", "ppPipelineState is null");
    return k_e_invalidarg;
  }
  if (!desc) {
    view_fail("CreateComputePipelineState", "pPipelineStateDesc is null");
    return k_e_invalidarg;
  }
  if (!root_sig_ok(desc->p_root_signature, reason, sizeof(reason))) {
    view_fail("CreateComputePipelineState", reason);
    return k_e_invalidarg;
  }
  if (!desc->cs.p_shader_bytecode && !desc->cs.bytecode_length) {
    view_fail("CreateComputePipelineState",
              "CS bytecode is null (a compute PSO needs a compute shader)");
    return k_e_invalidarg;
  }
  if (!shader_pair_ok(desc->cs, "CS", reason, sizeof(reason))) {
    view_fail("CreateComputePipelineState", reason);
    return k_e_invalidarg;
  }
  warn_dxbc(desc->cs, "CS");
  if ((desc->cached_blob != nullptr) != (desc->cached_blob_size != 0)) {
    view_fail("CreateComputePipelineState",
              "CachedPSO pointer/size are not paired");
    return k_e_invalidarg;
  }
  auto* p = new d3d12_simple_child();
  p->is_compute = true;
  p->root_sig = desc->p_root_signature;
  p->cs_blob = const_cast<void*>(desc->cs.p_shader_bytecode);
  p->cs_size = desc->cs.bytecode_length;
  return pso_finish(self, p, out_pso, "CreateComputePipelineState", 25);
}

// Walks the D3D12_PIPELINE_STATE_STREAM subobject stream: 16-byte headers
// {type@0, pad@4, size@8} followed by the 8-aligned payload. Garbage fails
// VISIBLY at the exact subobject.
int TRT_GUEST_ABI dev_create_pso_stream(void* self,
                                        const d3d12_stream_desc_t* desc,
                                        const void*, void** out_pso) {
  char reason[192];
  if (!out_pso) {
    view_fail("CreatePipelineState(stream)", "ppPipelineState is null");
    return k_e_invalidarg;
  }
  if (!desc) {
    view_fail("CreatePipelineState(stream)", "pPipelineStateDesc is null");
    return k_e_invalidarg;
  }
  if (!desc->p_subobject_stream || desc->size_in_bytes < 16) {
    view_fail("CreatePipelineState(stream)",
              "subobject stream is null or smaller than one 16-byte header");
    return k_e_invalidarg;
  }
  const u8* base = (const u8*)desc->p_subobject_stream;
  const u64 total = desc->size_in_bytes;
  u64 off = 0;
  bool have_vs = false, have_cs = false;
  void* rs_ptr = nullptr;
  const d3d12_shader_bytecode_t* vs = nullptr;
  const d3d12_shader_bytecode_t* ps = nullptr;
  const d3d12_shader_bytecode_t* cs = nullptr;
  while (off + 16 <= total) {
    const d3d12_subobject_hdr_t* hdr =
        (const d3d12_subobject_hdr_t*)(base + off);
    if (hdr->type >= k_subo_max_valid) {
      char r2[128];
      snprintf(r2, sizeof(r2),
               "subobject type %u outside 0..28 at stream offset %llu",
               hdr->type, (unsigned long long)off);
      view_fail("CreatePipelineState(stream)", r2);
      return k_e_invalidarg;
    }
    const u64 payload = off + 16;
    if (hdr->size > total - payload) {
      char r2[160];
      snprintf(r2, sizeof(r2),
               "subobject type %u payload of %llu bytes overruns the "
               "%llu-byte stream at offset %llu",
               hdr->type, (unsigned long long)hdr->size,
               (unsigned long long)total, (unsigned long long)off);
      view_fail("CreatePipelineState(stream)", r2);
      return k_e_invalidarg;
    }
    const void* data = base + payload;
    switch (hdr->type) {
      case k_subo_root_signature:
        if (hdr->size >= 8) rs_ptr = *(void* const*)data;
        break;
      case k_subo_vs:
        if (hdr->size < 16) {
          char r2[96];
          snprintf(r2, sizeof(r2),
                   "VS subobject payload is %llu bytes (needs 16)",
                   (unsigned long long)hdr->size);
          view_fail("CreatePipelineState(stream)", r2);
          return k_e_invalidarg;
        }
        vs = (const d3d12_shader_bytecode_t*)data;
        have_vs = true;
        break;
      case k_subo_ps:
        if (hdr->size >= 16) ps = (const d3d12_shader_bytecode_t*)data;
        break;
      case k_subo_cs:
        if (hdr->size < 16) {
          char r2[96];
          snprintf(r2, sizeof(r2),
                   "CS subobject payload is %llu bytes (needs 16)",
                   (unsigned long long)hdr->size);
          view_fail("CreatePipelineState(stream)", r2);
          return k_e_invalidarg;
        }
        cs = (const d3d12_shader_bytecode_t*)data;
        have_cs = true;
        break;
      default:
        break;
    }
    off = payload + ((hdr->size + 7u) & ~7u);
  }
  if (!have_vs && !have_cs) {
    view_fail("CreatePipelineState(stream)",
              "stream contains neither a VS nor a CS subobject");
    return k_e_invalidarg;
  }
  if (!root_sig_ok(rs_ptr, reason, sizeof(reason))) {
    view_fail("CreatePipelineState(stream)", reason);
    return k_e_invalidarg;
  }
  const d3d12_shader_bytecode_t* check[3] = {vs, ps, cs};
  const char* names[3] = {"VS", "PS", "CS"};
  for (int i = 0; i < 3; ++i) {
    if (check[i] && !shader_pair_ok(*check[i], names[i], reason,
                                    sizeof(reason))) {
      view_fail("CreatePipelineState(stream)", reason);
      return k_e_invalidarg;
    }
    if (check[i]) warn_dxbc(*check[i], names[i]);
  }
  auto* p = new d3d12_simple_child();
  p->is_compute = have_cs && !have_vs;
  p->root_sig = rs_ptr;
  if (vs) {
    p->vs_blob = const_cast<void*>(vs->p_shader_bytecode);
    p->vs_size = vs->bytecode_length;
  }
  if (ps) {
    p->ps_blob = const_cast<void*>(ps->p_shader_bytecode);
    p->ps_size = ps->bytecode_length;
  }
  if (cs) {
    p->cs_blob = const_cast<void*>(cs->p_shader_bytecode);
    p->cs_size = cs->bytecode_length;
  }
  return pso_finish(self, p, out_pso, "CreatePipelineState(stream)", 26);
}

int TRT_GUEST_ABI dev_create_cmdlist(void* self, u32, u32 type, void* alloc,
                                     void* pso, const void*, void** out_cl) {
  if (!alloc || !out_cl) return k_e_invalidarg;
  auto* cl = new d3d12_cmd_list();
  cl->vtbl = g_cmdlist_vtbl;
  cl->device = (d3d12_device*)self;
  cl->destroy_fn = &cmdlist_destroy;
  cl->type = type;
  cl->bound_pso = pso;
  {
    std::lock_guard<std::mutex> lk(g_live_mu);
    g_live_cmdlists.insert(cl);
  }
  *out_cl = cl;
  return k_s_ok;
}

int TRT_GUEST_ABI dev_create_cmdlist1(void* self, u32, u32 type, u32,
                                      const void*, void** out_cl) {
  if (!out_cl) return k_e_invalidarg;
  auto* cl = new d3d12_cmd_list();
  cl->vtbl = g_cmdlist_vtbl;
  cl->device = (d3d12_device*)self;
  cl->destroy_fn = &cmdlist_destroy;
  cl->type = type;
  cl->closed = true;
  {
    std::lock_guard<std::mutex> lk(g_live_mu);
    g_live_cmdlists.insert(cl);
  }
  *out_cl = cl;
  return k_s_ok;
}

int TRT_GUEST_ABI dev_check_feature(void* self, u32 feature, void* data, u32 sz) {
  if (!data) return k_e_invalidarg;
  auto* d = (d3d12_device*)self;
  if (feature == 0 && sz >= 16) {
    // D3D12_FEATURE_D3D12_OPTIONS: clean conservative baseline + resource binding tier 3
    memset(data, 0, sz);
    ((u32*)data)[2] = 3;  // ResourceBindingTier = TIER_3
    return k_s_ok;
  }
  if (feature == 2 && sz >= 8) {
    // D3D12_FEATURE_FEATURE_LEVELS
    struct fl_data { u32 num; const u32* list; u32 max_fl; };
    auto* fl = (fl_data*)data;
    u32 best = 0;
    if (fl->list) {
      for (u32 i = 0; i < fl->num; ++i)
        if (fl->list[i] <= d->max_fl && fl->list[i] > best) best = fl->list[i];
    }
    fl->max_fl = best ? best : d->max_fl;
    return best ? k_s_ok : k_dxgi_error_unsupported;
  }
  if (feature == 7 && sz >= 4) {
    // D3D12_FEATURE_SHADER_MODEL
    u32* sm = (u32*)data;
    if (*sm > 0x65u) *sm = 0x65u;
    return k_s_ok;
  }
  if (feature == 12 && sz >= 4) {
    // D3D12_FEATURE_ROOT_SIGNATURE: ROOT_SIGNATURE_VERSION_1_1 (0x2)
    u32* rs = (u32*)data;
    if (*rs > 2u) *rs = 2u;
    return k_s_ok;
  }
  memset(data, 0, sz);
  return k_s_ok;
}

int TRT_GUEST_ABI dev_create_descheap(void* self, const d3d12_heap_desc_info_t* desc,
                                      const void*, void** out_h) {
  if (!desc || !out_h) return k_e_invalidarg;
  if (desc->num_descriptors == 0) {
    trt_core_log(
        "d3d12: CreateDescriptorHeap -> E_INVALIDARG (NumDescriptors == 0)");
    return k_e_invalidarg;
  }
  if (desc->type > 3) {  // D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV..DSV
    char line[128];
    snprintf(line, sizeof(line),
             "d3d12: CreateDescriptorHeap -> E_INVALIDARG (heap type %u "
             "outside 0..3)",
             desc->type);
    trt_core_log(line);
    return k_e_invalidarg;
  }
  auto* h = new d3d12_desc_heap();
  h->vtbl = g_descheap_vtbl;
  h->device = (d3d12_device*)self;
  h->destroy_fn = &heap_desc_destroy;
  h->desc = *desc;
  h->inc = (desc->type == 0 || desc->type == 1) ? 32u : 64u;
  const u64 span = (u64)desc->num_descriptors * 64ULL;
  h->span = span;
  h->cpu_base = g_next_desc.fetch_add(span + 64ULL);
  if (desc->flags & 1u) h->gpu_base = h->cpu_base | 0x400000000000ULL;
  const u32 stored =
      desc->num_descriptors < k_desc_records_cap ? desc->num_descriptors
                                                 : k_desc_records_cap;
  if (desc->num_descriptors > k_desc_records_cap) {
    char line[160];
    snprintf(line, sizeof(line),
             "d3d12: CreateDescriptorHeap type %u num %u exceeds record "
             "storage cap %u - slots beyond the cap fail visibly",
             desc->type, desc->num_descriptors, k_desc_records_cap);
    d3d12_log_once(14, line);
  }
  h->records.resize(stored);
  for (auto& r : h->records) r.kind = k_desc_kind_empty;
  {
    std::lock_guard<std::mutex> lk(g_live_mu);
    g_live_heaps.insert(h);
  }
  *out_h = h;
  return k_s_ok;
}

u32 TRT_GUEST_ABI dev_desc_inc(void*, u32 type) {
  return (type == 0 || type == 1) ? 32u : 64u;
}

// ---- descriptor handle resolution + view/copy implementation ----------------
// handle = cpu_base + index*increment_size. The increment (32 for CSU/SAMPLER,
// 64 for RTV/DSV) — NOT the 64-byte VA span — is what decodes the index; the
// span is only reserved address space.
static bool is_live_resource(const void* p) {
  if (!p) return false;
  std::lock_guard<std::mutex> lk(g_live_mu);
  return g_live_resources.count(const_cast<void*>(p)) != 0;
}

static bool is_live_root_sig(const void* p) {
  if (!p) return false;
  std::lock_guard<std::mutex> lk(g_live_mu);
  return g_live_rootsigs.count(const_cast<void*>(p)) != 0;
}

static bool is_live_pso(const void* p) {
  if (!p) return false;
  std::lock_guard<std::mutex> lk(g_live_mu);
  return g_live_psos.count(const_cast<void*>(p)) != 0;
}

static bool is_live_cmdlist(const void* p) {
  if (!p) return false;
  std::lock_guard<std::mutex> lk(g_live_mu);
  return g_live_cmdlists.count(const_cast<void*>(p)) != 0;
}

// Resolves a CPU descriptor handle to (heap, index). Returns nullptr (with a
// human reason in `reason`) when the handle is not on a slot boundary of any
// live heap, is past the heap end, or is beyond record storage.
static d3d12_desc_heap* resolve_cpu_handle(u64 h, u32* out_idx, const char* api,
                                           char* reason, size_t reason_sz) {
  std::lock_guard<std::mutex> lk(g_live_mu);
  for (void* p : g_live_heaps) {
    auto* heap = (d3d12_desc_heap*)p;
    if (h < heap->cpu_base || h >= heap->cpu_base + heap->span) continue;
    const u64 off = h - heap->cpu_base;
    const u64 idx = off / heap->inc;
    if (off % heap->inc != 0) {
      snprintf(reason, reason_sz,
               "handle 0x%llx is offset %llu into heap type %u - not on a "
               "%u-byte slot boundary",
               (unsigned long long)h, (unsigned long long)off, heap->desc.type,
               heap->inc);
      (void)api;
      return nullptr;
    }
    if (idx >= heap->desc.num_descriptors) {
      snprintf(reason, reason_sz,
               "handle 0x%llx resolves to slot %llu of a %u-descriptor "
               "type-%u heap - past the heap end",
               (unsigned long long)h, (unsigned long long)idx,
               heap->desc.num_descriptors, heap->desc.type);
      return nullptr;
    }
    if (idx >= heap->records.size()) {
      snprintf(reason, reason_sz,
               "handle 0x%llx resolves to slot %llu - beyond descriptor "
               "record storage (cap %u)",
               (unsigned long long)h, (unsigned long long)idx,
               k_desc_records_cap);
      return nullptr;
    }
    if (out_idx) *out_idx = (u32)idx;
    return heap;
  }
  snprintf(reason, reason_sz,
           "handle 0x%llx not in any live descriptor heap",
           (unsigned long long)h);
  return nullptr;
}

// GPU handles are the CPU base | 0x400000000000 in this runtime; strip the
// synthetic bit and resolve as CPU.
static d3d12_desc_heap* resolve_gpu_handle(u64 h, u32* out_idx,
                                           const char* api, char* reason,
                                           size_t reason_sz) {
  return resolve_cpu_handle(h & ~0x400000000000ULL, out_idx, api, reason,
                            reason_sz);
}

static void view_fail(const char* api, const char* reason) {
  char line[256];
  snprintf(line, sizeof(line), "d3d12: %s -> E_INVALIDARG (%s)", api, reason);
  trt_core_log(line);
}

void TRT_GUEST_ABI dev_create_cbv(void* self, const d3d12_cbv_desc_t* desc,
                                  u64 handle) {
  (void)self;
  char reason[192];
  u32 idx = 0;
  if (!desc) {
    view_fail("CreateConstantBufferView", "pDesc is null");
    return;
  }
  auto* heap = resolve_cpu_handle(handle, &idx, "CreateConstantBufferView",
                                  reason, sizeof(reason));
  if (!heap) {
    view_fail("CreateConstantBufferView", reason);
    return;
  }
  if (desc->buffer_location == 0) {
    view_fail("CreateConstantBufferView",
              "D3D12_CONSTANT_BUFFER_VIEW_DESC.BufferLocation is 0 (a CBV "
              "needs a nonzero GPU virtual address)");
    return;
  }
  if (desc->size_in_bytes == 0) {
    view_fail("CreateConstantBufferView", "SizeInBytes is 0");
    return;
  }
  if (desc->size_in_bytes & 0xffu) {
    char line[160];
    snprintf(line, sizeof(line),
             "d3d12: CreateConstantBufferView SizeInBytes %u is not a "
             "256-byte multiple (accepted, recorded as-is)",
             desc->size_in_bytes);
    d3d12_log_once(9, line);
  }
  d3d12_view_record& r = heap->records[idx];
  r = d3d12_view_record{};
  r.kind = 0;  // CBV
  r.gpu_va = desc->buffer_location;
  r.size_bytes = desc->size_in_bytes;
  char line[160];
  snprintf(line, sizeof(line),
           "d3d12: CreateConstantBufferView recorded (heap type %u slot %u "
           "va 0x%llx size %u)",
           heap->desc.type, idx, (unsigned long long)desc->buffer_location,
           desc->size_in_bytes);
  d3d12_log_once(15, line);
}

#include "quality_policy.h"

// Texture quality tier, weak so an offline test binary without settings.cpp
// resolves it to null and keeps the default tier (full quality).
#if defined(__clang__) || defined(__GNUC__)
extern "C" int trt_settings_texture_quality_int(void) __attribute__((weak));
#endif

static int quality_tier() {
#if defined(__clang__) || defined(__GNUC__)
  if (trt_settings_texture_quality_int) return trt_settings_texture_quality_int();
#endif
  return 2;  // high: the guest's request passes through unchanged
}

// Parses the SRV/UAV union payload per ViewDimension into the record. The
// payload layouts are the SDK union arms (see the ABI probe asserts).
static bool parse_srv_payload(const d3d12_srv_desc_t* d, d3d12_view_record* r,
                              const char* api) {
  const u32* p = (const u32*)d->payload;
  r->mip = p[0];
  r->mip_levels = p[1];
  r->first_slice = p[2];
  r->num_slices = p[3];
  r->plane = p[4];
  switch (d->view_dimension) {
    case 1:  // BUFFER: {u64 FirstElement; u32 NumElements; u32 StructureByteStride; u32 Flags;}
      r->gpu_va = *(const u64*)d->payload;
      r->size_bytes = p[2];
      r->stride = p[3];
      r->flags = p[4];
      break;
    case 2: case 3: case 8: case 9:  // TEX1D/1DARRAY/3D/CUBE: mip/mips/clamp
      break;
    case 5:  // TEX2DARRAY: mip, mips, first, count, plane, clamp
      r->plane = p[4];
      break;
    case 4: case 6: case 7: case 10:  // TEX2D/MS/MSARRAY/CUBEARRAY
      break;
    case 11:  // RAYTRACING_ACCELERATION_STRUCTURE: {u64 Location;}
      r->gpu_va = *(const u64*)d->payload;
      break;
    default:
      view_fail(api, "SRV ViewDimension outside 1..11");
      return false;
  }
  return true;
}

void TRT_GUEST_ABI dev_create_srv(void* self, void* res,
                                  const d3d12_srv_desc_t* desc, u64 handle) {
  (void)self;
  char reason[192];
  u32 idx = 0;
  if (!desc) return;  // null desc + null resource = null-descriptor init slot
  if (!res) {
    view_fail("CreateShaderResourceView",
              "pResource is null with a non-null pDesc (a null-descriptor "
              "init requires BOTH pResource and pDesc to be null)");
    return;
  }
  if (!is_live_resource(res)) {
    snprintf(reason, sizeof(reason),
             "resource 0x%llx was not created by this device",
             (unsigned long long)(uintptr_t)res);
    view_fail("CreateShaderResourceView", reason);
    return;
  }
  auto* heap = resolve_cpu_handle(handle, &idx, "CreateShaderResourceView",
                                  reason, sizeof(reason));
  if (!heap) {
    view_fail("CreateShaderResourceView", reason);
    return;
  }
  d3d12_view_record r{};
  r.kind = 1;  // SRV
  r.format = desc->format;
  r.view_dim = desc->view_dimension;
  if (!parse_srv_payload(desc, &r, "CreateShaderResourceView")) return;
  r.resource = res;
  heap->records[idx] = r;
  char line[160];
  snprintf(line, sizeof(line),
           "d3d12: CreateShaderResourceView recorded (heap type %u slot %u "
           "dim %u res 0x%llx)",
           heap->desc.type, idx, desc->view_dimension,
           (unsigned long long)(uintptr_t)res);
  d3d12_log_once(16, line);
}

void TRT_GUEST_ABI dev_create_uav(void* self, void* res, void* counter_res,
                                  const d3d12_uav_desc_t* desc, u64 handle) {
  (void)self;
  char reason[192];
  u32 idx = 0;
  if (!desc) return;  // null-descriptor init slot
  if (!res) {
    view_fail("CreateUnorderedAccessView",
              "pResource is null with a non-null pDesc (a null-descriptor "
              "init requires BOTH pResource and pDesc to be null)");
    return;
  }
  if (!is_live_resource(res)) {
    snprintf(reason, sizeof(reason),
             "resource 0x%llx was not created by this device",
             (unsigned long long)(uintptr_t)res);
    view_fail("CreateUnorderedAccessView", reason);
    return;
  }
  if (counter_res && !is_live_resource(counter_res)) {
    snprintf(reason, sizeof(reason),
             "counter resource 0x%llx was not created by this device",
             (unsigned long long)(uintptr_t)counter_res);
    view_fail("CreateUnorderedAccessView", reason);
    return;
  }
  auto* heap = resolve_cpu_handle(handle, &idx, "CreateUnorderedAccessView",
                                  reason, sizeof(reason));
  if (!heap) {
    view_fail("CreateUnorderedAccessView", reason);
    return;
  }
  if (desc->view_dimension == 0 || desc->view_dimension > 8) {
    view_fail("CreateUnorderedAccessView",
              "UAV ViewDimension outside 1..8");
    return;
  }
  d3d12_view_record r{};
  r.kind = 2;  // UAV
  r.format = desc->format;
  r.view_dim = desc->view_dimension;
  const u32* p = (const u32*)desc->payload;
  r.mip = p[0];
  r.first_slice = p[2];
  r.num_slices = p[3];
  r.plane = p[4];
  if (desc->view_dimension == 1) {  // BUFFER
    r.gpu_va = *(const u64*)desc->payload;
    r.size_bytes = p[2];
    r.stride = p[3];
    r.flags = p[6];
  }
  r.resource = res;
  r.counter_resource = counter_res;
  heap->records[idx] = r;
  char line[160];
  snprintf(line, sizeof(line),
           "d3d12: CreateUnorderedAccessView recorded (heap type %u slot %u "
           "dim %u res 0x%llx)",
           heap->desc.type, idx, desc->view_dimension,
           (unsigned long long)(uintptr_t)res);
  d3d12_log_once(17, line);
}

void TRT_GUEST_ABI dev_create_rtv(void* self, void* res,
                                  const d3d12_rtv_desc_t* desc, u64 handle) {
  (void)self;
  char reason[192];
  u32 idx = 0;
  if (!res) {
    view_fail("CreateRenderTargetView",
              "pResource is null (RTV requires a resource)");
    return;
  }
  if (!is_live_resource(res)) {
    snprintf(reason, sizeof(reason),
             "resource 0x%llx was not created by this device",
             (unsigned long long)(uintptr_t)res);
    view_fail("CreateRenderTargetView", reason);
    return;
  }
  auto* heap = resolve_cpu_handle(handle, &idx, "CreateRenderTargetView",
                                  reason, sizeof(reason));
  if (!heap) {
    view_fail("CreateRenderTargetView", reason);
    return;
  }
  d3d12_view_record r{};
  r.kind = 3;  // RTV
  r.resource = res;
  if (desc) {
    r.format = desc->format;
    r.view_dim = desc->view_dimension;
    if (desc->view_dimension == 0 || desc->view_dimension > 8) {
      view_fail("CreateRenderTargetView", "RTV ViewDimension outside 1..8");
      return;
    }
    const u32* p = (const u32*)desc->payload;
    r.mip = p[0];
    r.first_slice = p[1];
    r.num_slices = p[2];
    r.plane = p[3];
    if (desc->view_dimension == 1) {  // BUFFER_RTV {u64 FirstElement; u32 NumElements;}
      r.gpu_va = *(const u64*)desc->payload;
      r.size_bytes = p[2];
    }
  } else {
    // pDesc == null is legal: derive the view from the resource itself.
    auto* rr = (d3d12_resource*)res;
    r.format = rr->desc.format;
    r.view_dim = rr->desc.dimension == 2 ? 2u    // TEX1D
                 : rr->desc.dimension == 3 ? 4u  // TEX2D
                 : rr->desc.dimension == 4 ? 8u  // TEX3D
                                           : 4u;
  }
  heap->records[idx] = r;
  char line[160];
  snprintf(line, sizeof(line),
           "d3d12: CreateRenderTargetView recorded (heap type %u slot %u "
           "dim %u res 0x%llx)",
           heap->desc.type, idx, r.view_dim,
           (unsigned long long)(uintptr_t)res);
  d3d12_log_once(18, line);
}

void TRT_GUEST_ABI dev_create_dsv(void* self, void* res,
                                  const d3d12_dsv_desc_t* desc, u64 handle) {
  (void)self;
  char reason[192];
  u32 idx = 0;
  if (!res) {
    view_fail("CreateDepthStencilView",
              "pResource is null (DSV requires a resource)");
    return;
  }
  if (!is_live_resource(res)) {
    snprintf(reason, sizeof(reason),
             "resource 0x%llx was not created by this device",
             (unsigned long long)(uintptr_t)res);
    view_fail("CreateDepthStencilView", reason);
    return;
  }
  auto* heap = resolve_cpu_handle(handle, &idx, "CreateDepthStencilView",
                                  reason, sizeof(reason));
  if (!heap) {
    view_fail("CreateDepthStencilView", reason);
    return;
  }
  d3d12_view_record r{};
  r.kind = 4;  // DSV
  r.resource = res;
  if (desc) {
    r.format = desc->format;
    r.view_dim = desc->view_dimension;
    r.flags = desc->flags;
    if (desc->view_dimension == 0 || desc->view_dimension > 6) {
      view_fail("CreateDepthStencilView", "DSV ViewDimension outside 1..6");
      return;
    }
    const u32* p = (const u32*)desc->payload;
    r.mip = p[0];
    r.first_slice = p[1];
    r.num_slices = p[2];
  } else {
    auto* rr = (d3d12_resource*)res;
    r.format = rr->desc.format;
    r.view_dim = rr->desc.dimension == 2 ? 1u    // TEX1D
                 : rr->desc.dimension == 3 ? 3u  // TEX2D
                                           : 3u;
  }
  heap->records[idx] = r;
  char line[160];
  snprintf(line, sizeof(line),
           "d3d12: CreateDepthStencilView recorded (heap type %u slot %u "
           "dim %u res 0x%llx)",
           heap->desc.type, idx, r.view_dim,
           (unsigned long long)(uintptr_t)res);
  d3d12_log_once(19, line);
}

void TRT_GUEST_ABI dev_create_sampler(void* self,
                                      const d3d12_sampler_desc_t* desc,
                                      u64 handle) {
  (void)self;
  char reason[192];
  u32 idx = 0;
  if (!desc) {
    view_fail("CreateSampler", "pDesc is null");
    return;
  }
  auto* heap = resolve_cpu_handle(handle, &idx, "CreateSampler", reason,
                                  sizeof(reason));
  if (!heap) {
    view_fail("CreateSampler", reason);
    return;
  }
  if (desc->filter > 0x1d5u) {  // D3D12_FILTER_MAXIMUM_ANISOTROPIC
    view_fail("CreateSampler", "Filter is not a valid D3D12_FILTER");
    return;
  }
  if (desc->max_anisotropy > 16u) {
    view_fail("CreateSampler", "MaxAnisotropy > 16");
    return;
  }
  if (desc->comparison_func > 8u) {
    view_fail("CreateSampler", "ComparisonFunc outside D3D12_COMPARISON_FUNC");
    return;
  }
  if (desc->address_u < 1u || desc->address_u > 5u ||
      desc->address_v < 1u || desc->address_v > 5u ||
      desc->address_w < 1u || desc->address_w > 5u) {
    view_fail("CreateSampler",
              "AddressU/V/W outside D3D12_TEXTURE_ADDRESS_MODE");
    return;
  }
  d3d12_view_record r{};
  r.kind = 5;  // SAMPLER
  r.sampler_filter = desc->filter;
  r.sampler_cmp = desc->comparison_func;
  // texture_quality can only lower what the guest asked for, never raise it,
  // so the default tier reproduces the guest's value exactly. This is the
  // consumer the setting never had.
  r.max_anisotropy = trt_quality_clamp_aniso(quality_tier(), desc->max_anisotropy);
  r.addr_u = desc->address_u;
  r.addr_v = desc->address_v;
  r.addr_w = desc->address_w;
  r.mip_lod_bias = desc->mip_lod_bias;
  r.min_lod = desc->min_lod;
  r.max_lod = desc->max_lod;
  for (int i = 0; i < 4; ++i) r.border_color[i] = desc->border_color[i];
  heap->records[idx] = r;
  char line[160];
  snprintf(line, sizeof(line),
           "d3d12: CreateSampler recorded (heap type %u slot %u filter 0x%x)",
           heap->desc.type, idx, desc->filter);
  d3d12_log_once(20, line);
}

void TRT_GUEST_ABI dev_copy_descriptors(void* self, u32 num_dst,
                                        const u64* dst_starts,
                                        const u32* dst_sizes, u32 num_src,
                                        const u64* src_starts,
                                        const u32* src_sizes, u32 heap_type) {
  (void)self;
  char reason[192];
  if (num_dst == 0 || !dst_starts) {
    view_fail("CopyDescriptors", "NumDestDescriptorRanges is 0 or the dest "
                                 "range array is null");
    return;
  }
  if (num_src == 0 || !src_starts) {
    view_fail("CopyDescriptors", "NumSrcDescriptorRanges is 0 or the src "
                                 "range array is null");
    return;
  }
  // Expand both sides to flat (heap, slot) lists with per-element handles
  // computed from each range's OWN heap increment.
  std::vector<std::pair<d3d12_desc_heap*, u32>> dst, src;
  u64 dst_total = 0, src_total = 0;
  for (u32 i = 0; i < num_dst; ++i) {
    const u32 count = dst_sizes ? dst_sizes[i] : 1u;
    u64 h = dst_starts[i];
    for (u32 j = 0; j < count; ++j) {
      u32 idx = 0;
      d3d12_desc_heap* heap =
          resolve_cpu_handle(h, &idx, "CopyDescriptors", reason, sizeof(reason));
      if (!heap) {
        view_fail("CopyDescriptors (dest)", reason);
        return;
      }
      dst.push_back({heap, idx});
      h += heap->inc;
      ++dst_total;
    }
  }
  for (u32 i = 0; i < num_src; ++i) {
    const u32 count = src_sizes ? src_sizes[i] : 1u;
    u64 h = src_starts[i];
    for (u32 j = 0; j < count; ++j) {
      u32 idx = 0;
      d3d12_desc_heap* heap =
          resolve_cpu_handle(h, &idx, "CopyDescriptors", reason, sizeof(reason));
      if (!heap) {
        view_fail("CopyDescriptors (src)", reason);
        return;
      }
      src.push_back({heap, idx});
      h += heap->inc;
      ++src_total;
    }
  }
  if (dst_total != src_total) {
    char line[192];
    snprintf(line, sizeof(line),
             "d3d12: CopyDescriptors -> E_INVALIDARG (dest total %llu != src "
             "total %llu descriptors)",
             (unsigned long long)dst_total, (unsigned long long)src_total);
    trt_core_log(line);
    return;
  }
  for (size_t i = 0; i < dst.size(); ++i) {
    dst[i].first->records[dst[i].second] = src[i].first->records[src[i].second];
  }
  char line[160];
  snprintf(line, sizeof(line),
           "d3d12: CopyDescriptors recorded (%llu descriptors, heap type %u)",
           (unsigned long long)dst_total, heap_type);
  d3d12_log_once(21, line);
}

void TRT_GUEST_ABI dev_copy_descriptors_simple(void* self, u32 num, u64 dst,
                                               u64 src, u32 heap_type) {
  (void)self;
  char reason[192];
  u32 dst_idx = 0, src_idx = 0;
  d3d12_desc_heap* dst_heap =
      resolve_cpu_handle(dst, &dst_idx, "CopyDescriptorsSimple", reason,
                         sizeof(reason));
  if (!dst_heap) {
    view_fail("CopyDescriptorsSimple (dest)", reason);
    return;
  }
  d3d12_desc_heap* src_heap =
      resolve_cpu_handle(src, &src_idx, "CopyDescriptorsSimple", reason,
                         sizeof(reason));
  if (!src_heap) {
    view_fail("CopyDescriptorsSimple (src)", reason);
    return;
  }
  if (num == 0) {
    view_fail("CopyDescriptorsSimple", "NumDescriptors is 0");
    return;
  }
  if ((u64)dst_idx + num > dst_heap->records.size() ||
      (u64)src_idx + num > src_heap->records.size()) {
    char line[192];
    snprintf(line, sizeof(line),
             "d3d12: CopyDescriptorsSimple -> E_INVALIDARG (range of %u "
             "descriptors from slot %u/%u runs past stored records)",
             num, dst_idx, src_idx);
    trt_core_log(line);
    return;
  }
  for (u32 i = 0; i < num; ++i) {
    dst_heap->records[dst_idx + i] = src_heap->records[src_idx + i];
  }
  char line[160];
  snprintf(line, sizeof(line),
           "d3d12: CopyDescriptorsSimple recorded (%u descriptors, heap type "
           "%u)",
           num, heap_type);
  d3d12_log_once(22, line);
}

// Device slot 42 = GetResourceTiling: void return, kept as a logged no-op
// (tiled-resource tiling queries are not part of this sprint).
void TRT_GUEST_ABI dev_view_noop(void*) {}

int TRT_GUEST_ABI dev_create_rootsig(void* self, u32, const void* blob, u64 len,
                                     const void*, void** out_rs) {
  if (!out_rs) {
    view_fail("CreateRootSignature", "ppvRootSignature is null");
    return k_e_invalidarg;
  }
  if (!blob || !len) {
    view_fail("CreateRootSignature",
              !blob ? "pBlobWithRootSignature is null"
                    : "blobLengthInBytes is 0");
    return k_e_invalidarg;
  }
  auto* rs = new d3d12_simple_child();
  rs->vtbl = g_child8_vtbl;
  rs->device = (d3d12_device*)self;
  rs->destroy_fn = &simple_child_destroy;
  rs->is_root_sig = true;
  char rs_err[192] = {0};
  if (rs_read_container(blob, len, &rs->rs_layout_store, rs_err, sizeof(rs_err)) == k_s_ok) {
    rs->rs_num_params = (u32)rs->rs_layout_store.params.size();
    rs->rs_num_samplers = (u32)(rs->rs_layout_store.samplers.size() / k_rssampler_size);
    rs->rs_flags = rs->rs_layout_store.flags;
    char line[192];
    snprintf(line, sizeof(line),
             "d3d12: CreateRootSignature accepted %u root parameter(s), %u static "
             "sampler(s), flags %#x (version %u, blob %llu bytes)",
             rs->rs_num_params, rs->rs_num_samplers, rs->rs_flags,
             rs->rs_layout_store.version, (unsigned long long)len);
    d3d12_log_once(23, line);
  } else {
    // Not a container this bridge wrote: a hand-authored test vector, or a blob
    // a real Windows d3d12.dll produced and the title cached. Accept it (the
    // corpus relies on that) but never quietly say the layout is known.
    char line[320];
    snprintf(line, sizeof(line),
             "d3d12: CreateRootSignature accepted an unrecognized blob (%llu bytes: %s) - "
             "layout not introspectable",
             (unsigned long long)len, rs_err);
    d3d12_log_once(24, line);
  }
  {
    std::lock_guard<std::mutex> lk(g_live_mu);
    g_live_rootsigs.insert(rs);
  }
  *out_rs = rs;
  return k_s_ok;
}

d3d12_alloc_info_t* TRT_GUEST_ABI dev_alloc_info(void*, d3d12_alloc_info_t* ret,
                                                 u32, u32 count,
                                                 const d3d12_resource_desc_t* descs) {
  if (!ret) return ret;
  u64 total = 0;
  for (u32 i = 0; i < count; ++i) {
    u64 w = descs ? (descs[i].width ? descs[i].width : 1) : 64;
    u64 h = descs ? (descs[i].height ? descs[i].height : 1) : 1;
    u64 sz = (descs && descs[i].dimension == 1) ? w : (w * h * 4ULL);
    total += (sz + 65535ULL) & ~65535ULL;
  }
  ret->size_in_bytes = total ? total : 65536ULL;
  ret->alignment = 65536ULL;
  return ret;
}

d3d12_heap_props_t* TRT_GUEST_ABI dev_custom_heap(void*, d3d12_heap_props_t* ret,
                                                  u32, u32 heap_type) {
  if (!ret) return ret;
  memset(ret, 0, sizeof(*ret));
  ret->type = heap_type;
  ret->cpu_page_prop = (heap_type == 2) ? 2 : 1;
  ret->memory_pool = 1;
  ret->creation_node_mask = 1;
  ret->visible_node_mask = 1;
  return ret;
}

int TRT_GUEST_ABI dev_create_resource(void* self, const d3d12_heap_props_t* props,
                                      u32 heap_flags, const d3d12_resource_desc_t* desc,
                                      u32, const void*, const void*, void** out_res) {
  if (!desc || !out_res) return k_e_invalidarg;
  auto* r = new d3d12_resource();
  r->vtbl = g_resource_vtbl;
  r->device = (d3d12_device*)self;
  r->destroy_fn = &resource_destroy;
  r->desc = *desc;
  if (props) r->heap_props = *props;
  r->heap_flags = heap_flags;
  const u64 sz = estimate_res_bytes(*desc);
  r->gpu_va = g_next_va.fetch_add(((sz + 65535ULL) & ~65535ULL) + 65536ULL);
  {
    std::lock_guard<std::mutex> lk(g_live_mu);
    g_live_resources.insert(r);
  }
  *out_res = r;
  return k_s_ok;
}

int TRT_GUEST_ABI dev_create_resource1(void* self, const d3d12_heap_props_t* props,
                                       u32 heap_flags, const d3d12_resource_desc_t* desc,
                                       u32 init_state, const void* clear_val,
                                       void* prot, const void* riid, void** out_res) {
  (void)prot;
  return dev_create_resource(self, props, heap_flags, desc, init_state, clear_val,
                             riid, out_res);
}

int TRT_GUEST_ABI dev_create_heap(void* self, const d3d12_heap_desc_t* desc,
                                  const void*, void** out_heap) {
  if (!desc || !out_heap) return k_e_invalidarg;
  auto* h = new d3d12_simple_child();
  h->vtbl = g_heap_vtbl;
  h->device = (d3d12_device*)self;
  h->destroy_fn = &simple_child_destroy;
  h->heap_desc = *desc;
  *out_heap = h;
  return k_s_ok;
}

int TRT_GUEST_ABI dev_create_heap1(void* self, const d3d12_heap_desc_t* desc,
                                   void*, const void* riid, void** out_heap) {
  return dev_create_heap(self, desc, riid, out_heap);
}

int TRT_GUEST_ABI dev_create_placed(void* self, void*, u64,
                                    const d3d12_resource_desc_t* desc, u32,
                                    const void*, const void* riid, void** out_res) {
  d3d12_heap_props_t p{};
  p.type = 1;
  return dev_create_resource(self, &p, 0, desc, 0, nullptr, riid, out_res);
}

int TRT_GUEST_ABI dev_unimpl(void*) { return k_e_notimpl; }

int TRT_GUEST_ABI dev_create_fence(void* self, u64 init_val, u32 flags,
                                   const void*, void** out_fence) {
  if (!out_fence) return k_e_invalidarg;
  auto* f = new d3d12_fence();
  f->vtbl = g_fence_vtbl;
  f->device = (d3d12_device*)self;
  f->destroy_fn = &fence_destroy;
  f->completed.store(init_val);
  f->flags = flags;
  *out_fence = f;
  return k_s_ok;
}

int TRT_GUEST_ABI dev_residency(void*, u32, void* const*) { return k_s_ok; }
int TRT_GUEST_ABI dev_removed_reason(void*) { return k_s_ok; }

void TRT_GUEST_ABI dev_footprints(void*, const d3d12_resource_desc_t* desc,
                                  u32 first_sub, u32 num_sub, u64 base_off,
                                  d3d12_placed_footprint_t* layouts,
                                  u32* num_rows, u64* row_bytes, u64* total_bytes) {
  const u32 w = (desc && desc->width) ? (u32)desc->width : 1u;
  const u32 h = (desc && desc->height) ? desc->height : 1u;
  const u32 bpp = (desc && desc->dimension == 1) ? 1u : 4u;
  const u32 rb = w * bpp;
  const u32 pitch = (rb + 255u) & ~255u;
  u64 off = base_off;
  for (u32 i = 0; i < num_sub; ++i) {
    const u32 mip = first_sub + i;
    const u32 mw = (w >> mip) ? (w >> mip) : 1u;
    const u32 mh = (desc && desc->dimension == 1) ? 1u : ((h >> mip) ? (h >> mip) : 1u);
    const u32 mrb = mw * bpp;
    const u32 mpitch = (mrb + 255u) & ~255u;
    if (layouts) {
      layouts[i].offset = off;
      layouts[i].format = desc ? desc->format : 0;
      layouts[i].width = mw;
      layouts[i].height = mh;
      layouts[i].depth = 1;
      layouts[i].row_pitch = mpitch;
    }
    if (num_rows) num_rows[i] = mh;
    if (row_bytes) row_bytes[i] = mrb;
    off += (u64)mpitch * mh;
    off = (off + 511ULL) & ~511ULL;
  }
  if (total_bytes) *total_bytes = off ? (off - base_off) : (u64)pitch * h;
}

int TRT_GUEST_ABI dev_create_simple8(void* self, const void*, const void*, void** out) {
  if (!out) return k_e_invalidarg;
  auto* o = new d3d12_simple_child();
  o->vtbl = g_child8_vtbl;
  o->device = (d3d12_device*)self;
  o->destroy_fn = &simple_child_destroy;
  *out = o;
  return k_s_ok;
}

int TRT_GUEST_ABI dev_create_cmdsig(void* self, const void* d, void*, const void* r, void** out) {
  return dev_create_simple8(self, d, r, out);
}

int TRT_GUEST_ABI dev_ok(void*) { return k_s_ok; }

d3d12_luid_t* TRT_GUEST_ABI dev_get_luid(void* self, d3d12_luid_t* ret) {
  if (ret) *ret = ((d3d12_device*)self)->luid;
  return ret;
}

int TRT_GUEST_ABI dev_unimpl_named(void*, const char* name) {
  char line[128];
  snprintf(line, sizeof(line), "d3d12: %s -> E_NOTIMPL (unimplemented)", name);
  trt_core_log(line);
  return k_e_notimpl;
}
int TRT_GUEST_ABI dev_create_lifetime_tracker(void*, void*, const void*, void**) {
  return dev_unimpl_named(nullptr, "ID3D12Device5::CreateLifetimeTracker");
}
void TRT_GUEST_ABI dev_remove_device(void*) {
  trt_core_log("d3d12: ID3D12Device5::RemoveDevice called");
}
int TRT_GUEST_ABI dev_enum_meta_cmds(void*, u32* num, void*) {
  if (num) *num = 0;
  return k_s_ok;
}
int TRT_GUEST_ABI dev_enum_meta_params(void*, const void*, u32, u32* num, void*) {
  if (num) *num = 0;
  return k_s_ok;
}
int TRT_GUEST_ABI dev_create_meta_cmd(void*, const void*, u32, const void*, u64, const void*, void**) {
  return dev_unimpl_named(nullptr, "ID3D12Device5::CreateMetaCommand");
}
int TRT_GUEST_ABI dev_create_state_obj(void*, const void*, const void*, void**) {
  return dev_unimpl_named(nullptr, "ID3D12Device5::CreateStateObject");
}
struct d3d12_rtas_prebuild_info_t {
  u64 result_data_max_size_in_bytes;
  u64 scratch_data_size_in_bytes;
  u64 update_scratch_data_size_in_bytes;
};
struct d3d12_build_rtas_inputs_t {
  u32 type;
  u32 flags;
  u32 num_descs;
};
void TRT_GUEST_ABI dev_get_rtas_prebuild_info(void*, const d3d12_build_rtas_inputs_t* inputs,
                                              d3d12_rtas_prebuild_info_t* out) {
  if (!out) return;
  const u64 n = (inputs && inputs->num_descs) ? inputs->num_descs : 1;
  out->result_data_max_size_in_bytes = ((n * 256ULL) + 255ULL) & ~255ULL;
  out->scratch_data_size_in_bytes = ((n * 512ULL) + 255ULL) & ~255ULL;
  out->update_scratch_data_size_in_bytes = ((n * 256ULL) + 255ULL) & ~255ULL;
}
u32 TRT_GUEST_ABI dev_check_driver_match(void*, u32, const void*) {
  return 0;  // D3D12_DRIVER_MATCHING_IDENTIFIER_COMPATIBLE_WITH_DEVICE
}
int TRT_GUEST_ABI dev_set_bg_proc_mode(void*, u32, u32, void*, int*) {
  return k_s_ok;
}
int TRT_GUEST_ABI dev_add_to_state_obj(void*, const void*, void*, const void*, void**) {
  return dev_unimpl_named(nullptr, "ID3D12Device7::AddToStateObject");
}
int TRT_GUEST_ABI dev_create_prot_res1(void*, const void*, const void*, void**) {
  return dev_unimpl_named(nullptr, "ID3D12Device7::CreateProtectedResourceSession1");
}
d3d12_alloc_info_t* TRT_GUEST_ABI dev_alloc_info2(void* self, d3d12_alloc_info_t* ret,
                                                  u32 visible_mask, u32 count,
                                                  const d3d12_resource_desc_t* descs,
                                                  void* info1_out) {
  (void)info1_out;
  return dev_alloc_info(self, ret, visible_mask, count, descs);
}
int TRT_GUEST_ABI dev_create_resource2(void* self, const d3d12_heap_props_t* props,
                                       u32 heap_flags, const d3d12_resource_desc_t* desc,
                                       u32 init_state, const void* clear_val,
                                       void* prot, u32, const u32*,
                                       const void* riid, void** out_res) {
  (void)prot;
  return dev_create_resource(self, props, heap_flags, desc, init_state, clear_val,
                             riid, out_res);
}
int TRT_GUEST_ABI dev_create_placed1(void* self, void* heap, u64 off,
                                     const d3d12_resource_desc_t* desc, u32 init_state,
                                     const void* clear_val, u32, const u32*,
                                     const void* riid, void** out_res) {
  return dev_create_placed(self, heap, off, desc, init_state, clear_val, riid, out_res);
}
void TRT_GUEST_ABI dev_create_sampler_feedback_uav(void*, void*, void*, u64) {
  trt_core_log("d3d12: ID3D12Device8::CreateSamplerFeedbackUnorderedAccessView (recorded no-op)");
}
void TRT_GUEST_ABI dev_footprints1(void* self, const d3d12_resource_desc_t* desc,
                                   u32 first_sub, u32 num_sub, u64 base_off,
                                   d3d12_placed_footprint_t* layouts,
                                   u32* num_rows, u64* row_bytes, u64* total_bytes) {
  dev_footprints(self, desc, first_sub, num_sub, base_off, layouts, num_rows,
                 row_bytes, total_bytes);
}
int TRT_GUEST_ABI dev_create_shader_cache(void*, const void*, const void*, void**) {
  return dev_unimpl_named(nullptr, "ID3D12Device9::CreateShaderCacheSession");
}
int TRT_GUEST_ABI dev_shader_cache_ctrl(void*, u32, u32) {
  return k_s_ok;
}
int TRT_GUEST_ABI dev_create_queue1(void* self, const d3d12_queue_desc_t* desc,
                                    const void* riid, const void*, void** out_q) {
  return dev_create_queue(self, desc, riid, out_q);
}

const void* g_device_vtbl[76] = {
    (const void*)&dev_qi,               // 0 QueryInterface
    (const void*)&base_addref,          // 1 AddRef
    (const void*)&base_release,         // 2 Release
    (const void*)&child_get_priv,       // 3 GetPrivateData
    (const void*)&child_set_priv,       // 4 SetPrivateData
    (const void*)&child_set_priv_iface, // 5 SetPrivateDataInterface
    (const void*)&child_set_name,       // 6 SetName
    (const void*)&dev_node_count,       // 7 GetNodeCount
    (const void*)&dev_create_queue,     // 8 CreateCommandQueue
    (const void*)&dev_create_alloc,     // 9 CreateCommandAllocator
    (const void*)&dev_create_pso_gfx,   // 10 CreateGraphicsPipelineState
    (const void*)&dev_create_pso_compute,  // 11 CreateComputePipelineState
    (const void*)&dev_create_cmdlist,   // 12 CreateCommandList
    (const void*)&dev_check_feature,    // 13 CheckFeatureSupport
    (const void*)&dev_create_descheap,  // 14 CreateDescriptorHeap
    (const void*)&dev_desc_inc,         // 15 GetDescriptorHandleIncrementSize
    (const void*)&dev_create_rootsig,   // 16 CreateRootSignature
    (const void*)&dev_create_cbv,       // 17 CreateConstantBufferView
    (const void*)&dev_create_srv,       // 18 CreateShaderResourceView
    (const void*)&dev_create_uav,       // 19 CreateUnorderedAccessView
    (const void*)&dev_create_rtv,       // 20 CreateRenderTargetView
    (const void*)&dev_create_dsv,       // 21 CreateDepthStencilView
    (const void*)&dev_create_sampler,   // 22 CreateSampler
    (const void*)&dev_copy_descriptors, // 23 CopyDescriptors
    (const void*)&dev_copy_descriptors_simple,  // 24 CopyDescriptorsSimple
    (const void*)&dev_alloc_info,       // 25 GetResourceAllocationInfo
    (const void*)&dev_custom_heap,      // 26 GetCustomHeapProperties
    (const void*)&dev_create_resource,  // 27 CreateCommittedResource
    (const void*)&dev_create_heap,      // 28 CreateHeap
    (const void*)&dev_create_placed,    // 29 CreatePlacedResource
    (const void*)&dev_unimpl,           // 30 CreateReservedResource
    (const void*)&dev_unimpl,           // 31 CreateSharedHandle
    (const void*)&dev_unimpl,           // 32 OpenSharedHandle
    (const void*)&dev_unimpl,           // 33 OpenSharedHandleByName
    (const void*)&dev_residency,        // 34 MakeResident
    (const void*)&dev_residency,        // 35 Evict
    (const void*)&dev_create_fence,     // 36 CreateFence
    (const void*)&dev_removed_reason,   // 37 GetDeviceRemovedReason
    (const void*)&dev_footprints,       // 38 GetCopyableFootprints
    (const void*)&dev_create_simple8,   // 39 CreateQueryHeap
    (const void*)&dev_ok,               // 40 SetStablePowerState
    (const void*)&dev_create_cmdsig,    // 41 CreateCommandSignature
    (const void*)&dev_view_noop,        // 42 GetResourceTiling
    (const void*)&dev_get_luid,         // 43 GetAdapterLuid
    (const void*)&dev_unimpl,           // 44 CreatePipelineLibrary
    (const void*)&dev_ok,               // 45 SetEventOnMultipleFenceCompletion
    (const void*)&dev_ok,               // 46 SetResidencyPriority
    (const void*)&dev_create_pso_stream,  // 47 CreatePipelineState
    (const void*)&dev_unimpl,           // 48 OpenExistingHeapFromAddress
    (const void*)&dev_unimpl,           // 49 OpenExistingHeapFromFileMapping
    (const void*)&dev_ok,               // 50 EnqueueMakeResident
    (const void*)&dev_create_cmdlist1,  // 51 CreateCommandList1
    (const void*)&dev_unimpl,           // 52 CreateProtectedResourceSession
    (const void*)&dev_create_resource1, // 53 CreateCommittedResource1
    (const void*)&dev_create_heap1,     // 54 CreateHeap1
    (const void*)&dev_unimpl,           // 55 CreateReservedResource1
    (const void*)&dev_alloc_info,       // 56 GetResourceAllocationInfo1
    // ---- ID3D12Device5..9 (slots 57-75) ----------------------------------
    (const void*)&dev_create_lifetime_tracker,     // 57 Device5 CreateLifetimeTracker
    (const void*)&dev_remove_device,               // 58 Device5 RemoveDevice
    (const void*)&dev_enum_meta_cmds,              // 59 Device5 EnumerateMetaCommands
    (const void*)&dev_enum_meta_params,            // 60 Device5 EnumerateMetaCommandParameters
    (const void*)&dev_create_meta_cmd,             // 61 Device5 CreateMetaCommand
    (const void*)&dev_create_state_obj,            // 62 Device5 CreateStateObject
    (const void*)&dev_get_rtas_prebuild_info,      // 63 Device5 GetRaytracingAccelerationStructurePrebuildInfo
    (const void*)&dev_check_driver_match,          // 64 Device5 CheckDriverMatchingIdentifier
    (const void*)&dev_set_bg_proc_mode,            // 65 Device6 SetBackgroundProcessingMode
    (const void*)&dev_add_to_state_obj,            // 66 Device7 AddToStateObject
    (const void*)&dev_create_prot_res1,            // 67 Device7 CreateProtectedResourceSession1
    (const void*)&dev_alloc_info2,                 // 68 Device8 GetResourceAllocationInfo2
    (const void*)&dev_create_resource2,            // 69 Device8 CreateCommittedResource2
    (const void*)&dev_create_placed1,              // 70 Device8 CreatePlacedResource1
    (const void*)&dev_create_sampler_feedback_uav, // 71 Device8 CreateSamplerFeedbackUnorderedAccessView
    (const void*)&dev_footprints1,                 // 72 Device8 GetCopyableFootprints1
    (const void*)&dev_create_shader_cache,         // 73 Device9 CreateShaderCacheSession
    (const void*)&dev_shader_cache_ctrl,           // 74 Device9 ShaderCacheControl
    (const void*)&dev_create_queue1,               // 75 Device9 CreateCommandQueue1
};
static_assert(sizeof(g_device_vtbl) / sizeof(g_device_vtbl[0]) == 76,
              "ID3D12Device9 vtbl must stay 76 slots (Win64 ABI)");

// ---- Agility SDK helper interfaces (SDKConfiguration / DeviceFactory) -----
struct simple_com {
  const void** vtbl;
  std::atomic<u32> refcount{1};
};
u32 TRT_GUEST_ABI scom_addref(void* s) { return ++((simple_com*)s)->refcount; }
u32 TRT_GUEST_ABI scom_rel(void* s) {
  auto* p = (simple_com*)s;
  const u32 r = --p->refcount;
  if (r == 0) delete p;
  return r;
}
int TRT_GUEST_ABI scom_qi(void* s, const void*, void** ppv) {
  if (!ppv) return k_e_invalidarg;
  *ppv = s;
  scom_addref(s);
  return k_s_ok;
}
int TRT_GUEST_ABI sdkcfg_set_ver(void*, u32, const char*) { return k_s_ok; }
const void* g_sdkcfg_vtbl[4] = {
    (const void*)&scom_qi, (const void*)&scom_addref,
    (const void*)&scom_rel, (const void*)&sdkcfg_set_ver,
};

int TRT_GUEST_ABI df_ok(void*) { return k_s_ok; }
int TRT_GUEST_ABI df_set_flags(void*, u32) { return k_s_ok; }
u32 TRT_GUEST_ABI df_get_flags(void*) { return 0; }
int TRT_GUEST_ABI df_get_cfg(void*, const void*, const void*, void** ppv) {
  if (ppv) *ppv = nullptr;
  return k_e_nointerface;
}
int TRT_GUEST_ABI df_exp(void*, u32, const void*, void*, const u32*) { return k_e_notimpl; }
int TRT_GUEST_ABI df_create_dev(void*, void* adapter, u32 min_fl, const void* riid, void** out_dev) {
  return trt_D3D12CreateDevice(adapter, min_fl, riid, out_dev);
}
const void* g_devfactory_vtbl[10] = {
    (const void*)&scom_qi, (const void*)&scom_addref, (const void*)&scom_rel,
    (const void*)&df_ok, (const void*)&df_ok, (const void*)&df_set_flags,
    (const void*)&df_get_flags, (const void*)&df_get_cfg,
    (const void*)&df_exp, (const void*)&df_create_dev,
};

}  // namespace

extern "C" {

void trt_d3d12_set_hooks(const trt_d3d12_hooks* hooks) {
  if (hooks) g_hooks = *hooks;
}

const trt_d3d12_hooks* trt_d3d12_hooks_get(void) { return &g_hooks; }

// TRIANGLE GATE accessor: how many command lists this queue has recorded via
// ExecuteCommandLists (slot 8). -1 if the pointer is not a bridge queue.
// The host test reads the queue pointer the GUEST created out of the guest's
// results file and asserts the submission actually landed host-side.
int trt_d3d12_queue_recorded_count(void* obj) {
  if (!obj) return -1;
  std::lock_guard<std::mutex> lk(g_track_mu);
  if (g_queues.count(obj) == 0) return -1;
  auto* q = (d3d12_queue*)obj;
  std::lock_guard<std::mutex> lk2(q->mu);
  return (int)q->recorded_lists.size();
}

int trt_d3d12_submission_count(void* obj) {
  if (!obj) return -1;
  std::lock_guard<std::mutex> lk(g_track_mu);
  if (g_queues.count(obj) == 0) return -1;
  auto* q = (d3d12_queue*)obj;
  std::lock_guard<std::mutex> lk2(q->mu);
  return (int)q->submissions.size();
}

static const d3d12_submission* d3d12_submission_at(void* obj, int index) {
  if (!obj || index < 0) return nullptr;
  if (g_queues.count(obj) == 0) return nullptr;
  auto* q = (d3d12_queue*)obj;
  if (index >= (int)q->submissions.size()) return nullptr;
  return &q->submissions[(size_t)index];
}

int trt_d3d12_submission_draws(void* obj, int index) {
  std::lock_guard<std::mutex> lk(g_track_mu);
  const auto* s = d3d12_submission_at(obj, index);
  return s ? (int)s->draws.size() : -1;
}

unsigned trt_d3d12_submission_vertices(void* obj, int index, int draw) {
  std::lock_guard<std::mutex> lk(g_track_mu);
  const auto* s = d3d12_submission_at(obj, index);
  if (!s || draw < 0 || draw >= (int)s->draws.size()) return 0;
  return s->draws[(size_t)draw].verts;
}

unsigned trt_d3d12_submission_instances(void* obj, int index, int draw) {
  std::lock_guard<std::mutex> lk(g_track_mu);
  const auto* s = d3d12_submission_at(obj, index);
  if (!s || draw < 0 || draw >= (int)s->draws.size()) return 0;
  return s->draws[(size_t)draw].insts;
}

// ---- D3D11-equivalent submission descriptors ------------------------------
// ExecuteCommandLists snapshots each list's recorded draws into a queue-local
// vector. This is the replay payload a host D3D11 submission would consume;
// it is intentionally data-only so it is gateable without a GPU, and never
// claims that pixels reached the screen.

// Descriptor-wave test accessors: host-side introspection of the descriptor
// heap record store and the command list's binding/copy state (same
// precedent as trt_d3d12_queue_recorded_count). Not guest-callable.
u32 trt_d3d12_heap_record_count(void* obj) {
  if (!obj) return 0;
  return (u32)((d3d12_desc_heap*)obj)->records.size();
}

int trt_d3d12_heap_record_kind(void* obj, u32 idx) {
  if (!obj) return -1;
  auto* h = (d3d12_desc_heap*)obj;
  if (idx >= h->records.size()) return -1;
  return (int)h->records[idx].kind;
}

u32 trt_d3d12_cmdlist_bound_heap_count(void* obj) {
  if (!obj) return 0;
  return (u32)((d3d12_cmd_list*)obj)->bound_heaps.size();
}

u32 trt_d3d12_cmdlist_root_table_count(void* obj) {
  if (!obj) return 0;
  return (u32)((d3d12_cmd_list*)obj)->root_tables.size();
}

u32 trt_d3d12_cmdlist_root_cbv_count(void* obj) {
  if (!obj) return 0;
  return (u32)((d3d12_cmd_list*)obj)->root_cbvs.size();
}

u32 trt_d3d12_cmdlist_copy_count(void* obj) {
  if (!obj) return 0;
  return ((d3d12_cmd_list*)obj)->copies;
}

u32 trt_d3d12_cmdlist_topology(void* obj) {
  return obj ? ((d3d12_cmd_list*)obj)->topology : 0;
}
void* trt_d3d12_cmdlist_pso(void* obj) {
  return obj ? ((d3d12_cmd_list*)obj)->bound_pso : nullptr;
}
void* trt_d3d12_cmdlist_gfx_rootsig(void* obj) {
  return obj ? ((d3d12_cmd_list*)obj)->gfx_rootsig : nullptr;
}
void* trt_d3d12_cmdlist_comp_rootsig(void* obj) {
  return obj ? ((d3d12_cmd_list*)obj)->comp_rootsig : nullptr;
}
u32 trt_d3d12_cmdlist_rtv_count(void* obj) {
  return obj ? ((d3d12_cmd_list*)obj)->rtv_count : 0;
}
u32 trt_d3d12_cmdlist_vb_binds(void* obj) {
  return obj ? ((d3d12_cmd_list*)obj)->vb_binds : 0;
}
u32 trt_d3d12_cmdlist_viewport_count(void* obj) {
  return obj ? ((d3d12_cmd_list*)obj)->viewport_count : 0;
}
u32 trt_d3d12_cmdlist_scissor_count(void* obj) {
  return obj ? ((d3d12_cmd_list*)obj)->scissor_count : 0;
}
u32 trt_d3d12_cmdlist_draws(void* obj) {
  return obj ? ((d3d12_cmd_list*)obj)->draws : 0;
}
u32 trt_d3d12_cmdlist_queries(void* obj) {
  return obj ? ((d3d12_cmd_list*)obj)->queries : 0;
}
u32 trt_d3d12_cmdlist_bundles(void* obj) {
  return obj ? ((d3d12_cmd_list*)obj)->bundles_executed : 0;
}

int trt_d3d12_is_queue(void* obj) {
  if (!obj) return 0;
  {
    std::lock_guard<std::mutex> lk(g_track_mu);
    if (g_queues.count(obj) > 0) return 1;
  }
  // Real UWP D3D12 queue: safe QueryInterface for IID_ID3D12CommandQueue
  if (!g_hooks.create_device) return 0;
  void* tmp = nullptr;
  const raw_guid k_iid_queue = {
      0x0ec870a6u, 0x5d7e, 0x4c22,
      {0x8c, 0xfc, 0x5b, 0xaa, 0xe0, 0x76, 0x16, 0xed}};
  using qi_fn = int (TRT_GUEST_ABI *)(void*, const void*, void**);
  using rel_fn = u32 (TRT_GUEST_ABI *)(void*);
  const void** vtbl = *(const void***)obj;
  if (!vtbl || !vtbl[0] || !vtbl[2]) return 0;
  if (((qi_fn)vtbl[0])(obj, &k_iid_queue, &tmp) == 0 && tmp) {
    ((rel_fn)vtbl[2])(tmp);
    return 1;
  }
  return 0;
}

int trt_d3d12_offline_backbuffer(u32 buffer_idx, u32 width, u32 height,
                                 const void* riid, void** out_res) {
  if (!out_res) return k_e_invalidarg;
  (void)buffer_idx; (void)riid;
  auto* r = new d3d12_resource();
  r->vtbl = g_resource_vtbl;
  r->destroy_fn = &resource_destroy;
  r->desc.dimension = 3;  // D3D12_RESOURCE_DIMENSION_TEXTURE2D
  r->desc.width = width ? width : 1280;
  r->desc.height = height ? height : 720;
  r->desc.depth_or_array = 1;
  r->desc.mip_levels = 1;
  r->desc.format = 87;  // DXGI_FORMAT_B8G8R8A8_UNORM
  r->desc.sample_count = 1;
  r->desc.flags = 1;  // D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
  r->gpu_va = g_next_va.fetch_add(0x400000ULL);
  {
    std::lock_guard<std::mutex> lk(g_live_mu);
    g_live_resources.insert(r);
  }
  *out_res = r;
  return k_s_ok;
}

int TRT_GUEST_ABI trt_D3D12CreateDevice(void* adapter, u32 min_fl,
                                      const void* riid, void** out_dev) {
  if (!is_valid_fl(min_fl)) return k_e_invalidarg;
  if (g_hooks.create_device) {
    return g_hooks.create_device(adapter, min_fl, riid, out_dev);
  }
  if (!out_dev) return k_s_false;  // probe only
  if (min_fl > k_fl_12_1) {
    *out_dev = nullptr;
    return k_dxgi_error_unsupported;
  }
  auto* dev = new d3d12_device();
  dev->vtbl = g_device_vtbl;
  dev->destroy_fn = &device_destroy;
  dev->max_fl = k_fl_12_1;
  *out_dev = dev;
  return k_s_ok;
}

int TRT_GUEST_ABI trt_D3D12GetDebugInterface(const void* riid, void** out_dbg) {
  if (g_hooks.get_debug_interface) return g_hooks.get_debug_interface(riid, out_dbg);
  if (out_dbg) *out_dbg = nullptr;
  trt_core_log("d3d12: D3D12GetDebugInterface -> E_NOINTERFACE (debug layer disabled)");
  return k_e_nointerface;
}

u32 trt_d3d12_rootsig_param_count(void* obj) {
  if (!obj) return 0;
  auto* rs = (d3d12_simple_child*)obj;
  return rs->is_root_sig ? rs->rs_num_params : 0;
}

u32 trt_d3d12_rootsig_static_sampler_count(void* obj) {
  if (!obj) return 0;
  auto* rs = (d3d12_simple_child*)obj;
  return rs->is_root_sig ? rs->rs_num_samplers : 0;
}

int trt_d3d12_rootsig_flags(void* obj) {
  if (!obj) return -1;
  auto* rs = (d3d12_simple_child*)obj;
  return rs->is_root_sig ? (int)rs->rs_flags : -1;
}

int TRT_GUEST_ABI trt_D3D12SerializeRootSignature(const void* desc, u32 version,
                                                  void** out_blob, void** out_err) {
  if (g_hooks.serialize_root_sig) {
    if (out_err) *out_err = nullptr;
    return g_hooks.serialize_root_sig(desc, version, out_blob, out_err);
  }
  return rs_serialize(desc, version, /*versioned=*/false, out_blob, out_err);
}

int TRT_GUEST_ABI trt_D3D12SerializeVersionedRootSignature(const void* desc,
                                                           void** out_blob,
                                                           void** out_err) {
  if (g_hooks.serialize_versioned_root_sig) {
    if (out_err) *out_err = nullptr;
    return g_hooks.serialize_versioned_root_sig(desc, out_blob, out_err);
  }
  return rs_serialize(desc, 0, /*versioned=*/true, out_blob, out_err);
}

int TRT_GUEST_ABI trt_D3D12CreateRootSignatureDeserializer(
    const void* src, u64 size, const void* riid, void** out_des) {
  if (g_hooks.create_root_deserializer)
    return g_hooks.create_root_deserializer(src, size, riid, out_des);
  return rs_create_deserializer(src, size, riid, /*versioned=*/false, out_des);
}

int TRT_GUEST_ABI trt_D3D12CreateVersionedRootSignatureDeserializer(
    const void* src, u64 size, const void* riid, void** out_des) {
  if (g_hooks.create_versioned_root_deserializer)
    return g_hooks.create_versioned_root_deserializer(src, size, riid, out_des);
  return rs_create_deserializer(src, size, riid, /*versioned=*/true, out_des);
}

int TRT_GUEST_ABI trt_D3D12GetInterface(const void* rclsid, const void* riid,
                                        void** out_iface) {
  if (g_hooks.get_interface) return g_hooks.get_interface(rclsid, riid, out_iface);
  if (!out_iface) return k_e_invalidarg;
  if (guid_eq(rclsid, 0x7cda6acau, 0xa03e, 0x49c8, 0x94, 0x58, 0x03, 0x34, 0xd2, 0x0e, 0x07, 0xce)) {
    auto* c = new simple_com{g_sdkcfg_vtbl};
    *out_iface = c;
    return k_s_ok;
  }
  if (guid_eq(rclsid, 0x114863bfu, 0xc386, 0x4aee, 0xb3, 0x9d, 0x8f, 0x0b, 0xbb, 0x06, 0x29, 0x55)) {
    auto* f = new simple_com{g_devfactory_vtbl};
    *out_iface = f;
    return k_s_ok;
  }
  *out_iface = nullptr;
  return k_e_nointerface;
}

int TRT_GUEST_ABI trt_D3D12EnableExperimentalFeatures(u32 num, const void* iids,
                                                      void* cfg, const u32* sizes) {
  if (g_hooks.enable_experimental) return g_hooks.enable_experimental(num, iids, cfg, sizes);
  return k_e_notimpl;
}

int TRT_GUEST_ABI trt_D3D12Core_unimpl(void) { return k_e_notimpl; }
void TRT_GUEST_ABI trt_D3D12PIX_noop(void) {}
int TRT_GUEST_ABI trt_D3D12Ord_noiface(void) { return k_e_nointerface; }

const trt_named_fn k_d3d12_fns[] = {
    {"D3D12CreateDevice", (void*)&trt_D3D12CreateDevice},
    {"D3D12GetDebugInterface", (void*)&trt_D3D12GetDebugInterface},
    {"D3D12SerializeRootSignature", (void*)&trt_D3D12SerializeRootSignature},
    {"D3D12SerializeVersionedRootSignature", (void*)&trt_D3D12SerializeVersionedRootSignature},
    {"D3D12CreateRootSignatureDeserializer", (void*)&trt_D3D12CreateRootSignatureDeserializer},
    {"D3D12CreateVersionedRootSignatureDeserializer", (void*)&trt_D3D12CreateVersionedRootSignatureDeserializer},
    {"D3D12GetInterface", (void*)&trt_D3D12GetInterface},
    {"D3D12EnableExperimentalFeatures", (void*)&trt_D3D12EnableExperimentalFeatures},
    {"D3D12CoreCreateLayeredDevice", (void*)&trt_D3D12Core_unimpl},
    {"D3D12CoreGetLayeredDeviceSize", (void*)&trt_D3D12Core_unimpl},
    {"D3D12CoreRegisterLayers", (void*)&trt_D3D12Core_unimpl},
    {"D3D12PIXEventsReplaceBlock", (void*)&trt_D3D12PIX_noop},
    {"D3D12PIXGetThreadInfo", (void*)&trt_D3D12PIX_noop},
    {"D3D12PIXNotifyWakeFromFenceSignal", (void*)&trt_D3D12PIX_noop},
    {"D3D12PIXReportCounter", (void*)&trt_D3D12PIX_noop},
    {"#101", (void*)&trt_D3D12Ord_noiface},
    {"#102", (void*)&trt_D3D12Ord_noiface},
    {"#103", (void*)&trt_D3D12Ord_noiface},
    {"#104", (void*)&trt_D3D12Ord_noiface},
    {"#105", (void*)&trt_D3D12Ord_noiface},
    {"#106", (void*)&trt_D3D12Ord_noiface},
};

int trt_bridge_register_d3d12(void) {
  // TRIANGLE GATE ABI guard: fail loudly (and visibly) if the CommandQueue
  // vtbl ever drifts from the real ID3D12CommandQueue layout again.
  if (!d3d12_vtbl_selfcheck()) {
    trt_core_log("d3d12: CommandQueue vtbl ABI check FAILED - refusing registration");
    return -1;
  }
  const int n = (int)(sizeof(k_d3d12_fns) / sizeof(k_d3d12_fns[0]));
  trt_module_register("d3d12.dll", TRT_ROUTE_BUILTIN, k_d3d12_fns, n);
  trt_module_register("d3d12core.dll", TRT_ROUTE_BUILTIN, k_d3d12_fns, n);
  return 0;
}

}  // extern "C"
