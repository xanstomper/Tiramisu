// tiramisu/src/guest/guest_triangle.c
// THE D3D12 TRIANGLE GATE guest (Sprint-2 item 21): a synthetic freestanding
// game that walks the FULL D3D12 render path offline, exactly like a real
// game's first frame:
//
//   1. D3D12CreateDevice               -> device
//   2. device->CreateCommandQueue      -> queue
//   3. device->CreateCommandAllocator  -> alloc
//   4. device->CreateCommandList       -> list
//   5. device->CreateRootSignature     -> rootsig
//   6. device->CreateGraphicsPipelineState (VS+PS blobs) -> pso
//   7. device->CreateCommittedResource (DEFAULT vertex buffer) -> res
//   8. list->SetGraphicsRootSignature / SetPipelineState / IASetVertexBuffers
//   9. list->DrawInstanced(3,1,0,0)
//  10. list->Close
//  11. queue->ExecuteCommandLists(1, &list)   <- BUG-1 proof (slot 8)
//  12. CreateFence + SetEventOnCompletion + queue->Signal -> event fires,
//      GetCompletedValue == 77
//  13. CreateDXGIFactory1 + factory->CreateSwapChainForCoreWindow (the D3D12
//      rule: the "device" passed to DXGI is the COMMAND QUEUE)
//  14. swapchain->Present(1,0)
//
// Every observable step ORs a distinct bit into the checksum (no carries);
// the host test asserts the exact value, prints the 12 ladder markers, and
// cross-checks host-side that the queue the guest created RECORDED the
// submission (trt_d3d12_queue_recorded_count on the pointer written to
// C:\results.txt). Runs through the full loader on the Linux host AND on
// console — the identical binary.
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned long long u64;
typedef long long i64;
typedef int i32;
typedef short i16;
typedef unsigned char u8;

#ifdef _WIN32
#define GSTD __stdcall
#else
#define GSTD
#endif

// d3d12.dll imports (builtin bridge)
__attribute__((dllimport)) i32 GSTD D3D12CreateDevice(void* adapter, u32 min_fl,
                                                      const void* riid,
                                                      void** out_dev);
// dxgi.dll imports (builtin bridge)
__attribute__((dllimport)) i32 GSTD CreateDXGIFactory1(const void* riid,
                                                       void** out_factory);
// kernel32 imports
__attribute__((dllimport)) void* GSTD CreateFileW(const u16* name, u32 access,
                                                  u32 share, void* sa,
                                                  u32 disp, u32 flags,
                                                  void* tmpl);
__attribute__((dllimport)) int GSTD WriteFile(void* h, const void* buf,
                                              u32 bytes, u32* written,
                                              void* ov);
__attribute__((dllimport)) int GSTD CloseHandle(void* h);
__attribute__((dllimport)) void* GSTD CreateEventW(void* attrs, int manual,
                                                   int initial, const u16* name);
__attribute__((dllimport)) u32 GSTD WaitForSingleObject(void* h, u32 ms);

// ---- Win64 ABI struct mirrors (byte-exact with the real headers) ----------
typedef struct { u32 type; i32 priority; u32 flags; u32 node_mask; }
    queue_desc;  // D3D12_COMMAND_QUEUE_DESC, 16 bytes

typedef struct {  // DXGI_SWAP_CHAIN_DESC1: 12 x u32 = 48 bytes
  u32 width, height, format, stereo, samp_count, samp_qual, usage, buffer_count,
      scaling, swap_effect, alpha_mode, flags;
} sc_desc1;

typedef struct {  // D3D12_RESOURCE_DESC: 56 bytes on Win64
  u32 dimension; u32 pad0; u64 alignment; u64 width; u32 height;
  u16 depth_or_array; u16 mip_levels; u32 format; u32 sample_count;
  u32 sample_quality; u32 layout; u32 flags; u32 pad1;
} res_desc;

typedef struct {  // D3D12_GRAPHICS_PIPELINE_STATE_DESC: 600 bytes on Win64
  const void* root_sig;                        // 0
  const void* vs_blob; u64 vs_len;             // 8
  const void* ps_blob; u64 ps_len;             // 24
  const void* ds_blob; u64 ds_len;             // 40
  const void* gs_blob; u64 gs_len;             // 56
  const void* so_decl; const void* so_strides; // 72
  u32 so_entries; u32 so_stride_count;         // 88
  u8 blend[328];                               // 96  (D3D12_BLEND_DESC)
  u32 sample_mask;                             // 424
  u8 rasterizer[40];                           // 428 (D3D12_RASTERIZER_DESC)
  u8 depth_stencil[56];                        // 468 (D3D12_DEPTH_STENCIL_DESC)
  const void* il_elems; u32 il_count; u32 il_pad; // 524
  u32 strip_cut;                               // 540
  u32 topo_type;                               // 544
  u32 num_rtv;                                 // 548
  u32 rtv_fmt[8];                              // 552
  u32 dsv_fmt;                                 // 584
  u32 samp_count; u32 samp_qual;               // 588
  u32 node_mask;                               // 596
} pso_desc;  // 600 bytes, 8-aligned

typedef struct { u64 gpu_va; u32 size; u32 stride; } vb_view;  // 16 bytes

// fake-but-plausible DXBC container bytes for the VS/PS blobs (the offline
// bridge validates pointer+length only; the shape keeps the guest honest
// about intent — a real game would embed compiled DXIL here)
static const u8 g_vs_blob[32] = {
    'D','X','B','C', 16,0,0,0, 'V','S','H',0, 1,0,1,0,
    0xDE,0xAD,0xBE,0xEF, 0x01,0x02,0x03,0x04, 0xAA,0xBB,0xCC,0xDD,
    0x55,0x66,0x77,0x88};
static const u8 g_ps_blob[32] = {
    'D','X','B','C', 16,0,0,0, 'P','S','H',0, 1,0,1,0,
    0xCA,0xFE,0xBA,0xBE, 0x10,0x20,0x30,0x40, 0xF0,0xE0,0xD0,0xC0,
    0x11,0x22,0x33,0x44};
// minimal root signature blob (bridge validates pointer+length only)
static const u8 g_rs_blob[16] = {
    0x44,0x58,0x53,0x52, 0x01,0x02,0x03,0x04,
    0xDE,0xC0,0xAD,0xDE, 0x00,0x01,0x02,0x03};

// three vertices of a triangle in clip space (the "game data" submitted)
static const float g_tri[9] = {0.0f, 0.5f, 0.5f, -0.5f, -0.5f, 0.5f,
                               0.5f, -0.5f, 0.5f};

// freestanding zero (no CRT: -fno-builtin keeps memset external)
static void zero(void* p, u32 n) {
  u8* b = (u8*)p;
  for (u32 i = 0; i < n; ++i) b[i] = 0;
}

u32 GSTD triangle_boot(u64 a, u64 b, u64 c) {
  (void)a; (void)b; (void)c;
  u32 sum = 0x40000000u;

  // 1. D3D12CreateDevice -> DEVICE OK
  void* dev = (void*)0;
  i32 hr = D3D12CreateDevice((void*)0, 0xC000u /*FL 12.0*/, (void*)0, &dev);
  if (hr == 0 && dev) sum |= 0x01u;

  // vtbl walk helper: *(void***)obj
  const void* const* dvt = dev ? *(const void* const**)dev : (void*)0;

  // 2. device slot 8 = CreateCommandQueue -> QUEUE OK
  void* q = (void*)0;
  queue_desc qd;
  typedef i32 (GSTD* cq_fn)(void*, const queue_desc*, const void*, void**);
  if (dvt) {
    hr = ((cq_fn)dvt[8])(dev, &qd, (void*)0, &q);
    if (hr == 0 && q) sum |= 0x02u;
  }

  // 3. device slot 9 = CreateCommandAllocator (type 0 = DIRECT) -> ALLOC OK
  void* alloc = (void*)0;
  typedef i32 (GSTD* ca_fn)(void*, u32, const void*, void**);
  if (dvt) {
    hr = ((ca_fn)dvt[9])(dev, 0u, (void*)0, &alloc);
    if (hr == 0 && alloc) sum |= 0x04u;
  }

  // 4. device slot 12 = CreateCommandList -> LIST OK
  void* cl = (void*)0;
  typedef i32 (GSTD* ccl_fn)(void*, u32, u32, void*, void*, const void*,
                             void**);
  if (dvt) {
    hr = ((ccl_fn)dvt[12])(dev, 0u, 0u, alloc, (void*)0, (void*)0, &cl);
    if (hr == 0 && cl) sum |= 0x08u;
  }

  // 5. device slot 16 = CreateRootSignature -> ROOTSIG OK
  void* rs = (void*)0;
  typedef i32 (GSTD* crs_fn)(void*, u32, const void*, u64, const void*,
                             void**);
  if (dvt) {
    hr = ((crs_fn)dvt[16])(dev, 0u, g_rs_blob, (u64)sizeof(g_rs_blob),
                           (void*)0, &rs);
    if (hr == 0 && rs) sum |= 0x10u;
  }

  // 6. device slot 10 = CreateGraphicsPipelineState -> PSO OK
  void* pso = (void*)0;
  typedef i32 (GSTD* cps_fn)(void*, const pso_desc*, const void*, void**);
  if (dvt) {
    pso_desc pd;
    zero(&pd, (u32)sizeof(pd));
    pd.root_sig = rs;
    pd.vs_blob = g_vs_blob; pd.vs_len = sizeof(g_vs_blob);
    pd.ps_blob = g_ps_blob; pd.ps_len = sizeof(g_ps_blob);
    pd.topo_type = 1u /*TRIANGLE*/;
    pd.num_rtv = 1u;
    pd.rtv_fmt[0] = 87u /*B8G8R8A8_UNORM*/;
    pd.samp_count = 1u;
    hr = ((cps_fn)dvt[10])(dev, &pd, (void*)0, &pso);
    if (hr == 0 && pso) sum |= 0x20u;
  }

  // 7. device slot 27 = CreateCommittedResource (DEFAULT vertex buffer)
  //    -> RESOURCE OK
  void* res = (void*)0;
  typedef i32 (GSTD* ccr_fn)(void*, const void*, u32, const res_desc*, u32,
                             const void*, const void*, void**);
  if (dvt) {
    // heap props: {Type=0(DEFAULT), CPU_PAGE_PROPERTY=0, MemoryPoolPreference=0,
    //              CreationNodeMask=1, VisibleNodeMask=1} = 5 x u32
    static const u32 heap_props[5] = {0, 0, 0, 1, 1};
    res_desc rd;
    zero(&rd, (u32)sizeof(rd));
    rd.dimension = 1u /*BUFFER*/;
    rd.width = sizeof(g_tri);
    rd.height = 1u;
    rd.depth_or_array = 1u;
    rd.mip_levels = 1u;
    rd.sample_count = 1u;
    rd.layout = 0u /*ROW_MAJOR not needed for buffers*/;
    hr = ((ccr_fn)dvt[27])(dev, heap_props, 0u, &rd,
                           0u /*COMMON_STATE*/, (void*)0, (void*)0, &res);
    if (hr == 0 && res) sum |= 0x40u;
  }

  // 8/9. list state + DrawInstanced(3,1,0,0) -> DRAW RECORDED (host log)
  const void* const* lvt = cl ? *(const void* const**)cl : (void*)0;
  if (lvt) {
    typedef void (GSTD* sgrs_fn)(void*, void*);
    typedef void (GSTD* sps_fn)(void*, void*);
    typedef void (GSTD* iavb_fn)(void*, u32, u32, const vb_view*);
    typedef void (GSTD* di_fn)(void*, u32, u32, u32, u32);
    ((sgrs_fn)lvt[30])(cl, rs);
    ((sps_fn)lvt[25])(cl, pso);
    // the vertex buffer's GPU address comes from the resource itself
    // (resource slot 11 = GetGPUVirtualAddress) — end-to-end honest
    typedef u64 (GSTD* gva_fn)(void*);
    u64 va = res ? ((gva_fn)(*(const void* const**)res)[11])(res) : 0ull;
    vb_view vb;
    vb.gpu_va = va;
    vb.size = (u32)sizeof(g_tri);
    vb.stride = 12u;
    ((iavb_fn)lvt[44])(cl, 0u, 1u, &vb);
    ((di_fn)lvt[12])(cl, 3u, 1u, 0u, 0u);
    sum |= 0x80u;
  }

  // 10. list slot 9 = Close -> LIST CLOSED
  if (lvt) {
    typedef i32 (GSTD* close_fn)(void*);
    hr = ((close_fn)lvt[9])(cl);
    if (hr == 0) sum |= 0x100u;
  }

  // 11. queue slot 8 = ExecuteCommandLists(1, &cl) -> ECL EXECUTED
  //     (BUG-1: this used to be a no-op UpdateTileMethods slot)
  const void* const* qvt = q ? *(const void* const**)q : (void*)0;
  if (qvt) {
    typedef void (GSTD* ecl_fn)(void*, u32, void* const*);
    ((ecl_fn)qvt[8])(q, 1u, &cl);
    sum |= 0x200u;
  }

  // 12. fence: CreateFence(slot 36) -> SetEventOnCompletion(77, ev) ->
  //     queue->Signal(77) -> event fires, GetCompletedValue == 77
  //     -> FENCE SIGNALED
  if (dvt && qvt) {
    void* fence = (void*)0;
    typedef i32 (GSTD* cf_fn)(void*, u64, u32, const void*, void**);
    hr = ((cf_fn)dvt[36])(dev, 0ull, 0u, (void*)0, &fence);
    if (hr == 0 && fence) {
      const void* const* fvt = *(const void* const**)fence;
      void* ev = CreateEventW((void*)0, 0, 0, (const u16*)0);
      typedef i32 (GSTD* seo_fn)(void*, u64, void*);
      typedef i32 (GSTD* sig_fn)(void*, void*, u64);
      typedef u64 (GSTD* gcv_fn)(void*);
      ((seo_fn)fvt[9])(fence, 77ull, ev);   // waiter queued (completed < 77)
      ((sig_fn)qvt[9])(q, fence, 77ull);    // signal fires the waiter
      const u32 waited = WaitForSingleObject(ev, 500);
      const u64 done = ((gcv_fn)fvt[8])(fence);
      if (waited == 0u /*WAIT_OBJECT_0*/ && done == 77ull) sum |= 0x400u;
      CloseHandle(ev);
    }
  }

  // 13. factory->CreateSwapChainForCoreWindow(queue, window, &desc1, ...)
  //     (DXGI D3D12 rule: pass the COMMAND QUEUE, not the device)
  //     -> SWAPCHAIN OK
  void* factory = (void*)0;
  void* sc = (void*)0;
  hr = CreateDXGIFactory1((void*)0, &factory);
  if (hr == 0 && factory) {
    const void* const* fvt = *(const void* const**)factory;
    sc_desc1 sd;
    zero(&sd, (u32)sizeof(sd));
    sd.width = 64u; sd.height = 64u;
    sd.format = 87u /*B8G8R8A8_UNORM*/;
    sd.samp_count = 1u;
    sd.usage = 0x20u /*RENDER_TARGET_OUTPUT*/;
    sd.buffer_count = 2u;
    sd.swap_effect = 4u /*FLIP_SEQUENTIAL*/;
    typedef i32 (GSTD* csc_fn)(void*, void*, void*, const sc_desc1*, void*, void**);
    // the "CoreWindow" is a dummy non-null IUnknown* — the offline shim
    // validates pointer-only (a real CoreWindow comes from the XAML island)
    static u32 dummy_window = 0xC0DEBA5Eu;
    hr = ((csc_fn)fvt[16])(factory, q, &dummy_window, &sd, (void*)0, &sc);
    if (hr == 0 && sc) sum |= 0x800u;
  }

  // 14. swapchain slot 8 = Present(1,0) -> PRESENTED
  if (sc) {
    const void* const* svt = *(const void* const**)sc;
    typedef i32 (GSTD* present_fn)(void*, u32, u32);
    hr = ((present_fn)svt[8])(sc, 1u, 0u);
    if (hr == 0) sum |= 0x1000u;
  }

  // results through the VFS: checksum + the queue pointer (the host asserts
  // the submission RECORDED host-side on the very object the guest created)
  void* f = CreateFileW(
      (const u16[]){'C',':','\\','r','e','s','u','l','t','s','.','t','x','t',0},
      0x40000000u /*GENERIC_WRITE*/, 0, (void*)0, 2 /*CREATE_ALWAYS*/, 0,
      (void*)0);
  if (f != (void*)-1 && f) {
    u32 written = 0;
    WriteFile(f, &sum, 4, &written, (void*)0);
    WriteFile(f, &q, 8, &written, (void*)0);
    CloseHandle(f);
  }
  return sum;
}
