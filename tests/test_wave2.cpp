// tiramisu/tests/test_wave2.cpp
// Wave 2 verification suite (Wave 2A: D3D12 + DXGI 1.6; extended in 2B/2C).
#include "../src/trt/trt.h"
#include "../src/trt/bridge_d3d.h"
#include "../src/trt/bridge_d3d12.h"
#include "../src/trt/bridge_d3d9.h"
#include "../src/trt/bridge_mem.h"
#include "../src/trt/bridge_msvcrt.h"
#include "../src/trt/bridge_ws2.h"
#include "../src/trt/images.h"

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <ucontext.h>
#include <unistd.h>
#include <string>
#include <vector>

extern "C" {
int trt_bridge_register_kernel32(void);
int trt_bridge_register_user32(void);
int TRT_GUEST_ABI trt_CreateDXGIFactory2(u32 flags, void* riid, void** out_factory);
u64 TRT_STDCALL trt_CreateEventW(void* sa, int manual, int init, const u16* name);
u32 TRT_STDCALL trt_WaitForSingleObject(u64 handle, u32 ms);
int TRT_STDCALL trt_CloseHandle(u64 handle);
}

namespace {

int g_failures = 0;
#define CHECK(cond, msg)                                             \
  do {                                                               \
    if (cond) printf("  PASS %s\n", msg);                            \
    else { printf("  FAIL %s\n", msg); ++g_failures; }               \
  } while (0)

struct raw_guid { u32 d1; u16 d2; u16 d3; u8 d4[8]; };

bool g_deny_rwx = false;

void* linux_alloc(size_t bytes, u64) {
  void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return p == MAP_FAILED ? nullptr : p;
}
int linux_protect(void* base, size_t bytes, unsigned prot) {
  if (g_deny_rwx &&
      (prot & (TRT_PROT_W | TRT_PROT_X)) == (TRT_PROT_W | TRT_PROT_X)) {
    return -1;
  }
  int mp = PROT_NONE;
  if (prot & TRT_PROT_R) mp |= PROT_READ;
  if (prot & TRT_PROT_W) mp |= PROT_WRITE;
  if (prot & TRT_PROT_X) mp |= PROT_EXEC;
  const uintptr_t s = (uintptr_t)base & ~(uintptr_t)4095ULL;
  const uintptr_t e = ((uintptr_t)base + bytes + 4095ULL) & ~(uintptr_t)4095ULL;
  return ::mprotect((void*)s, e - s, mp);
}
void linux_release(void* base, size_t bytes) { if (base) munmap(base, bytes); }
void linux_log(const char* line) { printf("  [trt] %s\n", line); }

void* linux_vmem_reserve(size_t bytes, u64 preferred) {
  void* p = mmap((void*)(uintptr_t)preferred, bytes, PROT_NONE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return p == MAP_FAILED ? nullptr : p;
}
int linux_vmem_commit(void* base, size_t bytes, unsigned prot) {
  return linux_protect(base, bytes, prot);
}
int linux_vmem_decommit(void* base, size_t bytes) {
  const uintptr_t s = (uintptr_t)base & ~(uintptr_t)4095ULL;
  const uintptr_t e = ((uintptr_t)base + bytes + 4095ULL) & ~(uintptr_t)4095ULL;
  ::madvise((void*)s, e - s, MADV_DONTNEED);
  return ::mprotect((void*)s, e - s, PROT_NONE);
}

void w2_rm_tree(const char* path) {
  struct stat st {};
  if (lstat(path, &st) != 0) return;
  if (S_ISDIR(st.st_mode)) {
    DIR* d = opendir(path);
    if (d) {
      while (struct dirent* e = readdir(d)) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        std::string sub = std::string(path) + "/" + e->d_name;
        w2_rm_tree(sub.c_str());
      }
      closedir(d);
    }
    rmdir(path);
  } else {
    unlink(path);
  }
}

void w2_write_file(const std::string& p, const void* data, size_t n) {
  FILE* f = fopen(p.c_str(), "wb");
  if (f) {
    if (n > 0) fwrite(data, 1, n, f);
    fclose(f);
  }
}

std::string w2_read_file(const std::string& p) {
  FILE* f = fopen(p.c_str(), "rb");
  if (!f) return {};
  std::string out;
  char buf[256];
  size_t r;
  while ((r = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, r);
  fclose(f);
  return out;
}

std::vector<u16> u16_lit(const char* s) {
  std::vector<u16> v;
  for (; *s; ++s) v.push_back((u8)*s);
  v.push_back(0);
  return v;
}

std::string u16_to_str(const u16* s) {
  std::string out;
  if (!s) return out;
  while (*s) {
    u16 c = *s++;
    out.push_back(c < 0x80u ? (char)c : '?');
  }
  return out;
}

struct vobj { const void** vtbl; };

template <typename Fn>
Fn slot(void* obj, int idx) {
  return (Fn)(((vobj*)obj)->vtbl[idx]);
}

void test_wave2a_d3d12() {
  trt_d3d12_set_hooks(nullptr);
  trt_gpu_hooks empty_hooks{};
  trt_gpu_set_hooks(&empty_hooks);

  CHECK(trt_module_route("d3d12.dll") == TRT_ROUTE_BUILTIN &&
            trt_module_query("d3d12.dll", "D3D12CreateDevice") == 1,
        "d3d12.dll D3D12CreateDevice registered BUILTIN");
  CHECK(trt_module_query_ordinal("d3d12.dll", 101) == 1,
        "d3d12.dll ordinal #101 registered BUILTIN");
  CHECK(trt_module_route("d3d12core.dll") == TRT_ROUTE_BUILTIN &&
            trt_module_query("d3d12core.dll", "D3D12GetInterface") == 1,
        "d3d12core.dll D3D12GetInterface registered BUILTIN");

  // 1. DXGI 1.6 factory & offline IDXGIAdapter4 enumeration
  const raw_guid iid_fac6 = {0xC1B6694Fu, 0xFF09, 0x44A9, {0xB0,0x3C,0x77,0x90,0x0A,0x0A,0x1D,0x17}};
  const raw_guid iid_adp4 = {0x3C96D98Du, 0x5E77, 0x4C22, {0x80,0x01,0x00,0x00,0x00,0x00,0x00,0x01}};
  void* fac = nullptr;
  CHECK(trt_CreateDXGIFactory2(0, (void*)&iid_fac6, &fac) == 0 && fac != nullptr,
        "CreateDXGIFactory2 returns IDXGIFactory6 offline");

  void* fac6 = nullptr;
  using qi_fn = int (TRT_GUEST_ABI *)(void*, const void*, void**);
  using rel_fn = u32 (TRT_GUEST_ABI *)(void*);
  CHECK(slot<qi_fn>(fac, 0)(fac, &iid_fac6, &fac6) == 0 && fac6 == fac,
        "IDXGIFactory::QueryInterface(IID_IDXGIFactory6) succeeds");

  using enum_gpu_fn = int (TRT_GUEST_ABI *)(void*, u32, u32, const void*, void**);
  void* adapter = nullptr;
  CHECK(slot<enum_gpu_fn>(fac6, 29)(fac6, 0, 2, &iid_adp4, &adapter) == 0 && adapter != nullptr,
        "IDXGIFactory6::EnumAdapterByGpuPreference(0, HIGH_PERFORMANCE) returns adapter");

  void* adapter2 = nullptr;
  CHECK(slot<enum_gpu_fn>(fac6, 29)(fac6, 1, 2, &iid_adp4, &adapter2) == (int)0x887A0002u,
        "EnumAdapterByGpuPreference(1) returns DXGI_ERROR_NOT_FOUND");

  struct adapter_desc1 {
    u16 desc[128];
    u32 vendor, dev, subsys, rev;
    u64 vram, sysmem, shared;
    u32 luid_lo; i32 luid_hi;
    u32 flags;
  } ad{};
  using get_desc1_fn = int (TRT_GUEST_ABI *)(void*, void*);
  CHECK(slot<get_desc1_fn>(adapter, 10)(adapter, &ad) == 0 &&
            ad.vendor == 0x1002u && ad.vram == 8ULL * 1024 * 1024 * 1024,
        "IDXGIAdapter4::GetDesc1 reports 8 GiB AMD RDNA2 class GPU");

  u32 tearing = 1;
  using check_feat_fn = int (TRT_GUEST_ABI *)(void*, u32, void*, u32);
  CHECK(slot<check_feat_fn>(fac6, 28)(fac6, 0, &tearing, sizeof(tearing)) == 0 && tearing == 0,
        "IDXGIFactory5::CheckFeatureSupport(ALLOW_TEARING) honestly reports FALSE");

  // 2. D3D12GetInterface Agility SDKConfiguration + DeviceFactory
  const raw_guid clsid_sdkcfg = {0x7cda6acau, 0xa03e, 0x49c8, {0x94,0x58,0x03,0x34,0xd2,0x0e,0x07,0xce}};
  const raw_guid clsid_devfac = {0x114863bfu, 0xc386, 0x4aee, {0xb3,0x9d,0x8f,0x0b,0xbb,0x06,0x29,0x55}};
  void* sdkcfg = nullptr;
  CHECK(trt_D3D12GetInterface(&clsid_sdkcfg, nullptr, &sdkcfg) == 0 && sdkcfg != nullptr,
        "D3D12GetInterface(CLSID_D3D12SDKConfiguration) succeeds");
  using set_sdk_fn = int (TRT_GUEST_ABI *)(void*, u32, const char*);
  CHECK(slot<set_sdk_fn>(sdkcfg, 3)(sdkcfg, 610, ".\\D3D12\\") == 0,
        "ID3D12SDKConfiguration::SetSDKVersion succeeds");
  slot<rel_fn>(sdkcfg, 2)(sdkcfg);

  void* devfac = nullptr;
  void* dev = nullptr;
  CHECK(trt_D3D12GetInterface(&clsid_devfac, nullptr, &devfac) == 0 && devfac != nullptr,
        "D3D12GetInterface(CLSID_D3D12DeviceFactory) succeeds");
  using df_create_fn = int (TRT_GUEST_ABI *)(void*, void*, u32, const void*, void**);
  CHECK(slot<df_create_fn>(devfac, 9)(devfac, adapter, 0xc000u, nullptr, &dev) == 0 && dev != nullptr,
        "ID3D12DeviceFactory::CreateDevice(FL_12_0) succeeds");
  slot<rel_fn>(devfac, 2)(devfac);

  // 3. CheckFeatureSupport (SHADER_MODEL 6.5), CommandQueue, SwapChain3, Resources, Fence
  u32 sm = 0x67u;
  CHECK(slot<check_feat_fn>(dev, 13)(dev, 7, &sm, sizeof(sm)) == 0 && sm == 0x65u,
        "ID3D12Device::CheckFeatureSupport(SHADER_MODEL) reports SM_6_5");

  struct { u32 type; i32 prio; u32 flags; u32 mask; } qdesc{0, 0, 0, 0};
  using create_q_fn = int (TRT_GUEST_ABI *)(void*, const void*, const void*, void**);
  void* queue = nullptr;
  CHECK(slot<create_q_fn>(dev, 8)(dev, &qdesc, nullptr, &queue) == 0 &&
            trt_d3d12_is_queue(queue) == 1,
        "ID3D12Device::CreateCommandQueue creates tracked ID3D12CommandQueue");

  struct { u32 w, h, fmt, stereo; u32 cnt, qual, usage, bufs, scaling, swap, alpha, flags; } scd{};
  scd.w = 1920; scd.h = 1080; scd.fmt = 28; scd.cnt = 1; scd.usage = 0x20; scd.bufs = 2; scd.swap = 4;
  void* sc = nullptr;
  using create_sc_hwnd_fn = int (TRT_GUEST_ABI *)(void*, void*, void*, void*, void*, void*, void**);
  CHECK(slot<create_sc_hwnd_fn>(fac6, 15)(fac6, queue, (void*)0x10101, &scd, nullptr, nullptr, &sc) == 0 && sc != nullptr,
        "IDXGIFactory2::CreateSwapChainForHwnd(D3D12 queue) succeeds");

  using bb_idx_fn = u32 (TRT_GUEST_ABI *)(void*);
  using get_buf_fn = int (TRT_GUEST_ABI *)(void*, u32, const void*, void**);
  using present_fn = int (TRT_GUEST_ABI *)(void*, u32, u32);
  CHECK(slot<bb_idx_fn>(sc, 36)(sc) == 0u,
        "IDXGISwapChain3::GetCurrentBackBufferIndex starts at 0");
  void* backbuf = nullptr;
  CHECK(slot<get_buf_fn>(sc, 9)(sc, 0, nullptr, &backbuf) == 0 && backbuf != nullptr,
        "IDXGISwapChain3::GetBuffer(0) returns offline ID3D12Resource");
  if (backbuf) slot<rel_fn>(backbuf, 2)(backbuf);
  CHECK(slot<present_fn>(sc, 8)(sc, 1, 0x200u /* ALLOW_TEARING stripped */) == 0 &&
            slot<bb_idx_fn>(sc, 36)(sc) == 1u,
        "IDXGISwapChain3::Present advances GetCurrentBackBufferIndex to 1");

  using create_alloc_fn = int (TRT_GUEST_ABI *)(void*, u32, const void*, void**);
  void* alloc = nullptr;
  CHECK(slot<create_alloc_fn>(dev, 9)(dev, 0, nullptr, &alloc) == 0 && alloc != nullptr,
        "ID3D12Device::CreateCommandAllocator succeeds");

  using create_cl_fn = int (TRT_GUEST_ABI *)(void*, u32, u32, void*, void*, const void*, void**);
  void* cl = nullptr;
  CHECK(slot<create_cl_fn>(dev, 12)(dev, 0, 0, alloc, nullptr, nullptr, &cl) == 0 && cl != nullptr,
        "ID3D12Device::CreateCommandList succeeds");

  struct { u32 type, num, flags, mask; } hdesc{0, 16, 1, 0};
  using create_dh_fn = int (TRT_GUEST_ABI *)(void*, const void*, const void*, void**);
  void* dheap = nullptr;
  CHECK(slot<create_dh_fn>(dev, 14)(dev, &hdesc, nullptr, &dheap) == 0 && dheap != nullptr,
        "ID3D12Device::CreateDescriptorHeap succeeds");

  using ret_u64_fn = u64* (TRT_GUEST_ABI *)(void*, u64*);
  u64 cpu_handle = 0, gpu_handle = 0;
  slot<ret_u64_fn>(dheap, 9)(dheap, &cpu_handle);
  slot<ret_u64_fn>(dheap, 10)(dheap, &gpu_handle);
  CHECK(cpu_handle != 0 && gpu_handle != 0,
        "ID3D12DescriptorHeap::GetCPU/GPUDescriptorHandleForHeapStart follow Win64 hidden-ret ABI");

  struct { u32 type, cpu, pool, cmask, vmask; } hprops{2 /* UPLOAD */, 0, 0, 1, 1};
  struct rdesc_t {
    u32 dim, pad0; u64 align, w; u32 h; u16 d, mip; u32 fmt, sc_cnt, sc_q, layout, flags, pad1;
  } rd{};
  rd.dim = 1; rd.w = 4096; rd.h = 1; rd.d = 1; rd.mip = 1; rd.sc_cnt = 1; rd.layout = 1;
  void* res = nullptr;
  using create_res_fn = int (TRT_GUEST_ABI *)(void*, const void*, u32, const void*, u32, const void*, const void*, void**);
  CHECK(slot<create_res_fn>(dev, 27)(dev, &hprops, 0, &rd, 0, nullptr, nullptr, &res) == 0 && res != nullptr,
        "ID3D12Device::CreateCommittedResource(4096B buffer) succeeds");

  using gpu_va_fn = u64 (TRT_GUEST_ABI *)(void*);
  using map_fn = int (TRT_GUEST_ABI *)(void*, u32, const void*, void**);
  using unmap_fn = void (TRT_GUEST_ABI *)(void*, u32, const void*);
  void* mapped = nullptr;
  CHECK(slot<gpu_va_fn>(res, 11)(res) != 0 &&
            slot<map_fn>(res, 8)(res, 0, nullptr, &mapped) == 0 && mapped != nullptr,
        "ID3D12Resource::GetGPUVirtualAddress + Map succeeds");
  if (mapped) {
    memcpy(mapped, "TIRAMISU_D3D12_OK", 18);
    slot<unmap_fn>(res, 9)(res, 0, nullptr);
    void* remapped = nullptr;
    slot<map_fn>(res, 8)(res, 0, nullptr, &remapped);
    CHECK(remapped && memcmp(remapped, "TIRAMISU_D3D12_OK", 18) == 0,
          "ID3D12Resource backing preserves mapped bytes across Unmap/Map");
  }

  struct { u32 n; const void* p; u32 ns; const void* s; u32 flags; } rs_desc{};
  void* rs_blob = nullptr;
  CHECK(trt_D3D12SerializeRootSignature(&rs_desc, 1, &rs_blob, nullptr) == 0 && rs_blob != nullptr,
        "D3D12SerializeRootSignature produces DXBC blob");
  using blob_ptr_fn = void* (TRT_GUEST_ABI *)(void*);
  using blob_sz_fn = u64 (TRT_GUEST_ABI *)(void*);
  const u32* magic = rs_blob ? (const u32*)slot<blob_ptr_fn>(rs_blob, 3)(rs_blob) : nullptr;
  CHECK(magic && *magic == 0x43425844u && slot<blob_sz_fn>(rs_blob, 4)(rs_blob) >= 40,
        "Serialized root signature blob begins with DXBC header");
  void* rootsig = nullptr;
  using create_rs_fn = int (TRT_GUEST_ABI *)(void*, u32, const void*, u64, const void*, void**);
  CHECK(slot<create_rs_fn>(dev, 16)(dev, 0, magic, slot<blob_sz_fn>(rs_blob, 4)(rs_blob), nullptr, &rootsig) == 0 && rootsig != nullptr,
        "ID3D12Device::CreateRootSignature succeeds");
  if (rs_blob) slot<rel_fn>(rs_blob, 2)(rs_blob);

  using cl_close_fn = int (TRT_GUEST_ABI *)(void*);
  using cl_draw_fn = void (TRT_GUEST_ABI *)(void*, u32, u32, u32, u32);
  using exec_fn = void (TRT_GUEST_ABI *)(void*, u32, void* const*);
  slot<cl_draw_fn>(cl, 12)(cl, 3, 1, 0, 0);
  CHECK(slot<cl_close_fn>(cl, 9)(cl) == 0, "ID3D12GraphicsCommandList::Close succeeds");
  slot<exec_fn>(queue, 8)(queue, 1, &cl);  // BUG-1 fix: ECL is slot 8

  void* fence = nullptr;
  using create_fence_fn = int (TRT_GUEST_ABI *)(void*, u64, u32, const void*, void**);
  using set_ev_fn = int (TRT_GUEST_ABI *)(void*, u64, void*);
  using q_sig_fn = int (TRT_GUEST_ABI *)(void*, void*, u64);
  using f_val_fn = u64 (TRT_GUEST_ABI *)(void*);
  CHECK(slot<create_fence_fn>(dev, 36)(dev, 0, 0, nullptr, &fence) == 0 && fence != nullptr,
        "ID3D12Device::CreateFence succeeds");
  u64 ev = trt_CreateEventW(nullptr, 0, 0, nullptr);
  CHECK(ev != 0, "CreateEventW for fence completion succeeds");
  slot<set_ev_fn>(fence, 9)(fence, 42, (void*)(uintptr_t)ev);
  CHECK(slot<q_sig_fn>(queue, 9)(queue, fence, 42) == 0 &&  // BUG-1: Signal = slot 9
            trt_WaitForSingleObject(ev, 500) == 0u &&
            slot<f_val_fn>(fence, 8)(fence) == 42ULL,
        "CommandQueue::Signal triggers Fence SetEventOnCompletion + updates CompletedValue");
  trt_CloseHandle(ev);

  slot<rel_fn>(fence, 2)(fence);
  if (rootsig) slot<rel_fn>(rootsig, 2)(rootsig);
  slot<rel_fn>(res, 2)(res);
  slot<rel_fn>(dheap, 2)(dheap);
  slot<rel_fn>(cl, 2)(cl);
  slot<rel_fn>(alloc, 2)(alloc);
  slot<rel_fn>(sc, 2)(sc);
  slot<rel_fn>(queue, 2)(queue);
  slot<rel_fn>(dev, 2)(dev);
  slot<rel_fn>(adapter, 2)(adapter);
  slot<rel_fn>(fac6, 2)(fac6);
  slot<rel_fn>(fac, 2)(fac);
}

void test_wave2b_mf() {
  CHECK(trt_module_route("mfplat.dll") == TRT_ROUTE_BUILTIN &&
            trt_module_route("mfreadwrite.dll") == TRT_ROUTE_BUILTIN &&
            trt_module_query("mfplat.dll", "MFStartup") == 1 &&
            trt_module_query("mfreadwrite.dll", "MFCreateSourceReaderFromURL") == 1,
        "Media Foundation (mfplat/mfreadwrite) registered BUILTIN");

  auto mf = [&](const char* mod, const char* sym) -> void* {
    return trt_resolve_module(nullptr, mod, sym, 0);
  };
  using MFStartup_t = i32 (TRT_GUEST_ABI *)(u32, u32);
  using MFShutdown_t = i32 (TRT_GUEST_ABI *)(void);
  using MFCreateAttributes_t = i32 (TRT_GUEST_ABI *)(void**, u32);
  using MFCreateMediaType_t = i32 (TRT_GUEST_ABI *)(void**);
  using MFCreateMemoryBuffer_t = i32 (TRT_GUEST_ABI *)(u32, void**);
  using MFCreateSample_t = i32 (TRT_GUEST_ABI *)(void**);
  using MFCreateSR_t = i32 (TRT_GUEST_ABI *)(const u16*, void*, void**);
  using Rel_t = u32 (TRT_GUEST_ABI *)(void*);

  auto pStartup = (MFStartup_t)mf("mfplat.dll", "MFStartup");
  auto pShutdown = (MFShutdown_t)mf("mfplat.dll", "MFShutdown");
  auto pAttrs = (MFCreateAttributes_t)mf("mfplat.dll", "MFCreateAttributes");
  auto pMT = (MFCreateMediaType_t)mf("mfplat.dll", "MFCreateMediaType");
  auto pBuf = (MFCreateMemoryBuffer_t)mf("mfplat.dll", "MFCreateMemoryBuffer");
  auto pSamp = (MFCreateSample_t)mf("mfplat.dll", "MFCreateSample");
  auto pSR = (MFCreateSR_t)mf("mfreadwrite.dll", "MFCreateSourceReaderFromURL");

  CHECK(pStartup && pStartup(0x00020070u, 0) == 0, "MFStartup succeeds");

  void* attr = nullptr;
  u8 k_test_guid[16] = {1, 2, 3, 4, 5, 6, 7, 8};
  u32 got_u32 = 0;
  using SetU32_t = i32 (TRT_GUEST_ABI *)(void*, const u8*, u32);
  using GetU32_t = i32 (TRT_GUEST_ABI *)(void*, const u8*, u32*);
  if (pAttrs && pAttrs(&attr, 4) == 0 && attr) {
    slot<SetU32_t>(attr, 21)(attr, k_test_guid, 0x1357u);
    slot<GetU32_t>(attr, 7)(attr, k_test_guid, &got_u32);
    slot<Rel_t>(attr, 2)(attr);
  }
  CHECK(got_u32 == 0x1357u, "IMFAttributes SetUINT32/GetUINT32 roundtrips");

  void* mt = nullptr;
  CHECK(pMT && pMT(&mt) == 0 && mt != nullptr, "MFCreateMediaType succeeds");
  if (mt) slot<Rel_t>(mt, 2)(mt);

  void* buf = nullptr;
  void* smp = nullptr;
  u32 total_len = 0;
  if (pBuf && pBuf(256, &buf) == 0 && buf && pSamp && pSamp(&smp) == 0 && smp) {
    using Lock_t = i32 (TRT_GUEST_ABI *)(void*, u8**, u32*, u32*);
    using Unlock_t = i32 (TRT_GUEST_ABI *)(void*);
    using SetLen_t = i32 (TRT_GUEST_ABI *)(void*, u32);
    using AddBuf_t = i32 (TRT_GUEST_ABI *)(void*, void*);
    using TotLen_t = i32 (TRT_GUEST_ABI *)(void*, u32*);
    u8* ptr = nullptr;
    if (slot<Lock_t>(buf, 3)(buf, &ptr, nullptr, nullptr) == 0 && ptr) {
      memcpy(ptr, "PCM", 4);
      slot<Unlock_t>(buf, 4)(buf);
      slot<SetLen_t>(buf, 6)(buf, 128);
    }
    slot<AddBuf_t>(smp, 42)(smp, buf);
    slot<TotLen_t>(smp, 45)(smp, &total_len);
    slot<Rel_t>(buf, 2)(buf);
    slot<Rel_t>(smp, 2)(smp);
  }
  CHECK(total_len == 128u, "MFCreateMemoryBuffer + IMFSample preserves buffer length");

  void* sr = nullptr;
  void* sr_mt = nullptr;
  u32 act_stream = 99, sflags = 0;
  void* out_samp = (void*)0x1;
  const u16 k_url[] = {'Z', ':', '\\', 'i', 'n', 't', 'r', 'o', '.', 'm', 'p', '4', 0};
  i32 rs_hr = -1;
  if (pSR && pSR(k_url, nullptr, &sr) == 0 && sr) {
    using GetCurMT_t = i32 (TRT_GUEST_ABI *)(void*, u32, void**);
    using ReadSamp_t = i32 (TRT_GUEST_ABI *)(void*, u32, u32, u32*, u32*, i64*, void**);
    slot<GetCurMT_t>(sr, 6)(sr, 0xFFFFFFFCu /*MF_SOURCE_READER_FIRST_VIDEO_STREAM*/, &sr_mt);
    rs_hr = slot<ReadSamp_t>(sr, 9)(sr, 0xFFFFFFFCu, 0, &act_stream, &sflags, nullptr, &out_samp);
    if (sr_mt) slot<Rel_t>(sr_mt, 2)(sr_mt);
    slot<Rel_t>(sr, 2)(sr);
  }
  CHECK(sr != nullptr && sr_mt != nullptr && rs_hr == 0 && (sflags & 0x2u) != 0 &&
            out_samp == nullptr,
        "IMFSourceReader GetCurrentMediaType + ReadSample ends stream cleanly (EOS)");
  CHECK(pShutdown && pShutdown() == 0, "MFShutdown succeeds");
}

void w2_segv_handler(int sig, siginfo_t* si, void* uc) {
  if (trt_dispatch_hw_fault(sig, si, uc)) return;
  _exit(139);
}

void test_wave2c_vfs_cow() {
  char ro_tpl[] = "/tmp/trt_w2_ro_XXXXXX";
  char c_tpl[] = "/tmp/trt_w2_c_XXXXXX";
  char* ro_dir = mkdtemp(ro_tpl);
  char* c_dir = mkdtemp(c_tpl);
  CHECK(ro_dir != nullptr && c_dir != nullptr, "VFS temp dirs created");
  if (!ro_dir || !c_dir) return;

  std::string ro_sub = std::string(ro_dir) + "/sub";
  std::string ro_ddel = std::string(ro_dir) + "/dir_del";
  mkdir(ro_sub.c_str(), 0755);
  mkdir(ro_ddel.c_str(), 0755);
  w2_write_file(ro_sub + "/ro_only.txt", "RO", 2);
  w2_write_file(std::string(ro_dir) + "/keep.txt", "RO_KEEP", 7);
  w2_write_file(std::string(ro_dir) + "/gone.txt", "RO_DEL", 6);
  w2_write_file(ro_ddel + "/old.txt", "OLD", 3);

  trt_vfs_mount("c:", c_dir);
  trt_vfs_mount("z:", ro_dir);

  auto k32 = [&](const char* sym) -> void* {
    return trt_resolve_module(nullptr, "kernel32.dll", sym, 0);
  };
  auto crt = [&](const char* sym) -> void* {
    return trt_resolve_module(nullptr, "msvcrt.dll", sym, 0);
  };

  using Ultoa = char* (TRT_GUEST_ABI*)(u32, char*, i32);
  using Strspn = u64 (TRT_GUEST_ABI*)(const char*, const char*);
  using Tell = i64 (TRT_GUEST_ABI*)(i32);
  using Open = void* (TRT_GUEST_ABI*)(const char*, const char*);
  using Getc = i32 (TRT_GUEST_ABI*)(void*);
  auto ultoa = reinterpret_cast<Ultoa>(crt("_ultoa"));
  auto strspn_fn = reinterpret_cast<Strspn>(crt("strspn"));
  auto tell = reinterpret_cast<Tell>(crt("_telli64"));
  auto open_file = reinterpret_cast<Open>(crt("fopen"));
  auto file_number = reinterpret_cast<Getc>(crt("_fileno"));
  using Read = i32 (TRT_GUEST_ABI*)(i32, void*, u32);
  auto read_bytes = reinterpret_cast<Read>(crt("_read"));
  auto close_file = reinterpret_cast<Getc>(crt("fclose"));
  char digits[33];
  CHECK(ultoa && ultoa(0xffffffffu, digits, 10) == digits &&
        strcmp(digits, "4294967295") == 0, "CRT converts Windows unsigned long");
  CHECK(strspn_fn && strspn_fn("aac", "ab") == 2, "CRT strspn uses the requested character set");
  void* stream = open_file ? open_file("Z:\\keep.txt", "rb") : nullptr;
  const i32 fd = stream && file_number ? file_number(stream) : -1;
  char first = 0;
  CHECK(fd >= 0 && tell && read_bytes && read_bytes(fd, &first, 1) == 1 &&
        first == 'R' && tell(fd) == 1, "CRT _telli64 uses the guest descriptor");
  if (stream && close_file) close_file(stream);

  using SetDirW_t = int (TRT_STDCALL *)(const u16*);
  using GetDirW_t = u32 (TRT_STDCALL *)(u32, u16*);
  using SetDirA_t = int (TRT_STDCALL *)(const char*);
  using GetDirA_t = u32 (TRT_STDCALL *)(u32, char*);
  using CreateFileW_t = void* (TRT_STDCALL *)(const u16*, u32, u32, void*, u32, u32, void*);
  using WriteFile_t = int (TRT_STDCALL *)(void*, const void*, u32, u32*, void*);
  using ReadFile_t = int (TRT_STDCALL *)(void*, void*, u32, u32*, void*);
  using CloseHandlePtr_t = int (TRT_STDCALL *)(void*);
  using DelFileW_t = int (TRT_STDCALL *)(const u16*);
  using RmDirW_t = int (TRT_STDCALL *)(const u16*);
  using MkDirW_t = int (TRT_STDCALL *)(const u16*, void*);
  using MoveExW_t = int (TRT_STDCALL *)(const u16*, const u16*, u32);
  using FindFirstW_t = void* (TRT_STDCALL *)(const u16*, void*);
  using FindNextW_t = int (TRT_STDCALL *)(void*, void*);
  using FindClose_t = int (TRT_STDCALL *)(void*);

  auto pSetDirW = (SetDirW_t)k32("SetCurrentDirectoryW");
  auto pGetDirW = (GetDirW_t)k32("GetCurrentDirectoryW");
  auto pSetDirA = (SetDirA_t)k32("SetCurrentDirectoryA");
  auto pGetDirA = (GetDirA_t)k32("GetCurrentDirectoryA");
  auto pCreateFileW = (CreateFileW_t)k32("CreateFileW");
  auto pWriteFile = (WriteFile_t)k32("WriteFile");
  auto pReadFile = (ReadFile_t)k32("ReadFile");
  auto pCloseH = (CloseHandlePtr_t)k32("CloseHandle");
  auto pDelW = (DelFileW_t)k32("DeleteFileW");
  auto pRmDirW = (RmDirW_t)k32("RemoveDirectoryW");
  auto pMkDirW = (MkDirW_t)k32("CreateDirectoryW");
  auto pMoveExW = (MoveExW_t)k32("MoveFileExW");
  auto pFind1W = (FindFirstW_t)k32("FindFirstFileW");
  auto pFindNW = (FindNextW_t)k32("FindNextFileW");
  auto pFindClose = (FindClose_t)k32("FindClose");

  // Full CWD + relative .. folding + \\?\ prefix stripping
  auto w_sub = u16_lit("Z:\\sub\\.");
  CHECK(pSetDirW && pSetDirW(w_sub.data()) == 1, "SetCurrentDirectoryW Z:\\sub");
  u16 cwd_w[260] = {};
  if (pGetDirW) pGetDirW(260, cwd_w);
  CHECK(u16_to_str(cwd_w) == "Z:\\sub", "GetCurrentDirectoryW returns Z:\\sub");

  char norm_a[256] = {};
  auto w_rel = u16_lit("..\\sub\\.\\ro_only.txt");
  trt_vfs_normalize_guest_utf16(w_rel.data(), norm_a, sizeof(norm_a));
  CHECK(strcmp(norm_a, "Z:\\sub\\ro_only.txt") == 0,
        "Relative path + '..'/'.' normalization yields Z:\\sub\\ro_only.txt");

  char norm_ext[256] = {};
  auto w_ext = u16_lit("\\\\?\\Z:\\sub\\..\\keep.txt");
  trt_vfs_normalize_guest_utf16(w_ext.data(), norm_ext, sizeof(norm_ext));
  CHECK(strcmp(norm_ext, "Z:\\keep.txt") == 0,
        "\\\\?\\ prefix stripping + '..' folding yields Z:\\keep.txt");

  CHECK(pSetDirA && pSetDirA("Z:\\sub\\..") == 1, "SetCurrentDirectoryA Z:\\sub\\..");
  char cwd_a[260] = {};
  if (pGetDirA) pGetDirA(260, cwd_a);
  CHECK(strcmp(cwd_a, "Z:\\") == 0, "GetCurrentDirectoryA folds back to Z:\\");

  // Z: CoW write & delete whiteout without mutating read-only Z: root
  auto w_gone = u16_lit("Z:\\gone.txt");
  CHECK(pDelW && pDelW(w_gone.data()) == 1, "DeleteFileW Z:\\gone.txt whites out");
  CHECK(w2_read_file(std::string(ro_dir) + "/gone.txt") == "RO_DEL",
        "Read-only Z: host gone.txt untouched by CoW delete");
  void* h_gone = pCreateFileW(w_gone.data(), 0x80000000u, 1, nullptr, 3, 0x80, nullptr);
  CHECK(h_gone == (void*)(intptr_t)-1, "CreateFileW(OPEN_EXISTING) on whited-out file fails");

  // CoW write over existing Z: file
  auto w_keep = u16_lit("Z:\\keep.txt");
  void* hw = pCreateFileW(w_keep.data(), 0x40000000u, 0, nullptr, 2 /*CREATE_ALWAYS*/, 0x80, nullptr);
  u32 nw = 0;
  if (hw != (void*)(intptr_t)-1) {
    pWriteFile(hw, "COW_NEW", 7, &nw, nullptr);
    pCloseH(hw);
  }
  char rb[32] = {};
  u32 nr = 0;
  void* hr = pCreateFileW(w_keep.data(), 0x80000000u, 1, nullptr, 3, 0x80, nullptr);
  if (hr != (void*)(intptr_t)-1) {
    pReadFile(hr, rb, 7, &nr, nullptr);
    pCloseH(hr);
  }
  CHECK(nr == 7 && memcmp(rb, "COW_NEW", 7) == 0 &&
            w2_read_file(std::string(ro_dir) + "/keep.txt") == "RO_KEEP",
        "CoW write shadows Z:\\keep.txt while leaving read-only source untouched");

  // Opaque directory: RemoveDirectoryW + CreateDirectoryW hides old RO child
  auto w_ddel_old = u16_lit("Z:\\dir_del\\old.txt");
  auto w_ddel = u16_lit("Z:\\dir_del");
  pDelW(w_ddel_old.data());
  CHECK(pRmDirW && pRmDirW(w_ddel.data()) == 1, "RemoveDirectoryW Z:\\dir_del succeeds");
  CHECK(pMkDirW && pMkDirW(w_ddel.data(), nullptr) == 1,
        "CreateDirectoryW recreates Z:\\dir_del as opaque CoW directory");
  void* h_old = pCreateFileW(w_ddel_old.data(), 0x80000000u, 1, nullptr, 3, 0x80, nullptr);
  CHECK(h_old == (void*)(intptr_t)-1, "Opaque dir blocks stale Z:\\dir_del\\old.txt");

  // FindFirstFileW / FindNextFileW merges CoW overlay + RO and skips whiteouts
  auto w_ovnew = u16_lit("Z:\\ov_added.txt");
  void* ho = pCreateFileW(w_ovnew.data(), 0x40000000u, 0, nullptr, 2, 0x80, nullptr);
  if (ho != (void*)(intptr_t)-1) { pWriteFile(ho, "OV", 2, &nw, nullptr); pCloseH(ho); }

  auto w_pat = u16_lit("Z:\\*");
  u8 wfd[592] = {};
  std::vector<std::string> found_names;
  void* hf = pFind1W(w_pat.data(), wfd);
  if (hf != (void*)(intptr_t)-1) {
    do {
      found_names.push_back(u16_to_str((const u16*)(wfd + 44)));
    } while (pFindNW(hf, wfd));
    pFindClose(hf);
  }
  auto has_name = [&](const char* target) -> bool {
    int count = 0;
    for (const auto& s : found_names) if (s == target) ++count;
    return count == 1;
  };
  auto absent_name = [&](const char* target) -> bool {
    for (const auto& s : found_names) if (s == target) return false;
    return true;
  };
  CHECK(has_name("keep.txt") && has_name("ov_added.txt") && absent_name("gone.txt"),
        "FindFirstFileW merges CoW+RO cleanly without duplicates or deleted whiteouts");

  // MoveFileExW moves RO-backed file cleanly via CoW without mutating RO
  auto w_mv_dst = u16_lit("Z:\\moved_sub.txt");
  auto w_mv_src = u16_lit("Z:\\sub\\ro_only.txt");
  CHECK(pMoveExW && pMoveExW(w_mv_src.data(), w_mv_dst.data(), 1) == 1,
        "MoveFileExW Z:\\sub\\ro_only.txt -> Z:\\moved_sub.txt succeeds");
  CHECK(w2_read_file(std::string(ro_dir) + "/sub/ro_only.txt") == "RO",
        "Read-only source Z:\\sub\\ro_only.txt preserved on host after MoveFileExW");
  void* h_old_src = pCreateFileW(w_mv_src.data(), 0x80000000u, 1, nullptr, 3, 0x80, nullptr);
  void* h_new_dst = pCreateFileW(w_mv_dst.data(), 0x80000000u, 1, nullptr, 3, 0x80, nullptr);
  if (h_new_dst != (void*)(intptr_t)-1) pCloseH(h_new_dst);
  CHECK(h_old_src == (void*)(intptr_t)-1 && h_new_dst != (void*)(intptr_t)-1,
        "MoveFileExW hides moved source and reads moved destination on CoW overlay");

  // msvcrt _chdir / _getcwd / _wgetenv / _wputenv_s / _wfindfirst64
  using crt_chdir_t = i32 (TRT_GUEST_ABI *)(const char*);
  using crt_getcwd_t = char* (TRT_GUEST_ABI *)(char*, i32);
  using crt_wputenv_s_t = i32 (TRT_GUEST_ABI *)(const u16*, const u16*);
  using crt_wgetenv_t = u16* (TRT_GUEST_ABI *)(const u16*);
  using crt_wfind1_t = i64 (TRT_GUEST_ABI *)(const u16*, void*);
  using crt_findclose_t = i32 (TRT_GUEST_ABI *)(i64);

  auto pChdir = (crt_chdir_t)crt("_chdir");
  auto pGetcwd = (crt_getcwd_t)crt("_getcwd");
  auto pWputenvS = (crt_wputenv_s_t)crt("_wputenv_s");
  auto pWgetenv = (crt_wgetenv_t)crt("_wgetenv");
  auto pWfind1 = (crt_wfind1_t)crt("_wfindfirst64");
  auto pCrtFindClose = (crt_findclose_t)crt("_findclose");

  char crt_cwd[260] = {};
  CHECK(pChdir && pChdir("Z:\\sub") == 0 && pGetcwd && pGetcwd(crt_cwd, 260) != nullptr &&
            strcmp(crt_cwd, "Z:\\sub") == 0,
        "msvcrt _chdir/_getcwd tracks VFS CWD");

  auto w_ek = u16_lit("TRT_W2_TEST_ENV");
  auto w_ev = u16_lit("crt_value_42");
  u16* got_ev = (pWputenvS && pWgetenv && pWputenvS(w_ek.data(), w_ev.data()) == 0)
                    ? pWgetenv(w_ek.data())
                    : nullptr;
  CHECK(got_ev && u16_to_str(got_ev) == "crt_value_42",
        "msvcrt _wputenv_s/_wgetenv roundtrips UTF-16 env");

  u8 wfd64[560] = {};
  i64 fh = pWfind1 ? pWfind1(w_mv_dst.data(), wfd64) : -1;
  std::string f64_name = (fh != -1) ? u16_to_str((const u16*)(wfd64 + 40)) : "";
  if (fh != -1 && pCrtFindClose) pCrtFindClose(fh);
  CHECK(fh != -1 && f64_name == "moved_sub.txt" && *(i64*)(wfd64 + 32) == 2,
        "msvcrt _wfindfirst64 finds CoW file with exact name and 64-bit size");

  trt_vfs_set_cwd("Z:\\");
  w2_rm_tree(ro_dir);
  w2_rm_tree(c_dir);
}

void test_wave2d_vmem_jit() {
  trt_set_vmem_hooks(&linux_vmem_reserve, &linux_vmem_commit, &linux_vmem_decommit);
  auto k32 = [&](const char* sym) -> void* {
    return trt_resolve_module(nullptr, "kernel32.dll", sym, 0);
  };
  using VA_t = void* (TRT_STDCALL *)(void*, u64, u32, u32);
  using VP_t = int (TRT_STDCALL *)(void*, u64, u32, u32*);
  using VF_t = int (TRT_STDCALL *)(void*, u64, u32);
  using VQ_t = u64 (TRT_STDCALL *)(const void*, void*, u64);
  using FlushIC_t = int (TRT_STDCALL *)(void*, const void*, u64);

  auto pVA = (VA_t)k32("VirtualAlloc");
  auto pVP = (VP_t)k32("VirtualProtect");
  auto pVF = (VF_t)k32("VirtualFree");
  auto pVQ = (VQ_t)k32("VirtualQuery");
  auto pFlush = (FlushIC_t)k32("FlushInstructionCache");

  struct mbi64 {
    u64 BaseAddress;
    u64 AllocationBase;
    u32 AllocationProtect;
    u16 PartitionId;
    u16 _pad;
    u64 RegionSize;
    u32 State;
    u32 Protect;
    u32 Type;
    u32 _pad2;
  };

  void* rsv = pVA(nullptr, 65536, 0x2000u /*MEM_RESERVE*/, 0x01u /*PAGE_NOACCESS*/);
  mbi64 m{};
  u64 vqn = rsv ? pVQ(rsv, &m, sizeof(m)) : 0;
  CHECK(rsv != nullptr && vqn == sizeof(m) && m.State == 0x2000u && m.RegionSize >= 65536u,
        "MEM_RESERVE VirtualAlloc + VirtualQuery reports reserved region");

  u8* sub = (u8*)rsv + 4096;
  void* cmt = pVA(sub, 8192, 0x1000u /*MEM_COMMIT*/, 0x04u /*PAGE_READWRITE*/);
  if (cmt) memcpy(cmt, "SUBPAGE_OK", 10);
  mbi64 m_sub{};
  pVQ(sub, &m_sub, sizeof(m_sub));
  CHECK(cmt == sub && memcmp(sub, "SUBPAGE_OK", 10) == 0 &&
            m_sub.State == 0x1000u && m_sub.Protect == 0x04u,
        "Sparse MEM_COMMIT sub-range writes data and updates VirtualQuery extent");

  u32 oldp = 0;
  CHECK(pVP(sub + 4096, 4096, 0x02u /*PAGE_READONLY*/, &oldp) == 1 && oldp == 0x04u,
        "Sub-extent VirtualProtect transitions page and returns previous protect");
  mbi64 m_split{};
  pVQ(sub + 4096, &m_split, sizeof(m_split));
  CHECK(m_split.Protect == 0x02u, "VirtualQuery splits extent on sub-range VirtualProtect");

  CHECK(pVF(sub, 4096, 0x4000u /*MEM_DECOMMIT*/) == 1, "MEM_DECOMMIT sub-page succeeds");
  mbi64 m_dec{};
  pVQ(sub, &m_dec, sizeof(m_dec));
  CHECK(m_dec.State == 0x2000u, "VirtualQuery reports MEM_RESERVE after MEM_DECOMMIT");
  pVF(rsv, 0, 0x8000u /*MEM_RELEASE*/);

  // Two-phase W^X JIT: ACG denies RWX -> fault promotion AND FlushInstructionCache
  struct sigaction sa {}, old_sa {};
  sa.sa_sigaction = w2_segv_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, &old_sa);

  g_deny_rwx = true;
  u8* jit1 = (u8*)pVA(nullptr, 4096, 0x3000u, 0x04u);
  u8* jit2 = (u8*)pVA(nullptr, 4096, 0x3000u, 0x04u);
  int res1 = -1, res2 = -1;
  if (jit1 && jit2) {
    // mov eax, 42; ret
    static const u8 k_code1[6] = {0xB8, 42, 0, 0, 0, 0xC3};
    // mov eax, 99; ret
    static const u8 k_code2[6] = {0xB8, 99, 0, 0, 0, 0xC3};
    memcpy(jit1, k_code1, sizeof(k_code1));
    memcpy(jit2, k_code2, sizeof(k_code2));

    u32 prev1 = 0, prev2 = 0;
    int vp1 = pVP(jit1, 4096, 0x40u /*PAGE_EXECUTE_READWRITE*/, &prev1);
    int vp2 = pVP(jit2, 4096, 0x40u /*PAGE_EXECUTE_READWRITE*/, &prev2);
    // Path 1: NX instruction fetch triggers SIGSEGV -> trt_dispatch_hw_fault promotes RX
    if (vp1) res1 = ((int (*)(void))jit1)();
    // Path 2: FlushInstructionCache promotes RX upfront before execution
    if (vp2 && pFlush(nullptr, jit2, 4096) == 1) res2 = ((int (*)(void))jit2)();
    CHECK(vp1 == 1 && res1 == 42, "W^X two-phase JIT fault promotion executes RX code -> 42");
    CHECK(vp2 == 1 && res2 == 99, "W^X FlushInstructionCache promotes RX upfront -> 99");
  }
  if (jit1) pVF(jit1, 0, 0x8000u);
  if (jit2) pVF(jit2, 0, 0x8000u);
  g_deny_rwx = false;
  sigaction(SIGSEGV, &old_sa, nullptr);
}

std::vector<u8> build_min_pe64_dll() {
  std::vector<u8> pe(1536, 0);
  auto w16 = [&](size_t off, u16 v) { memcpy(&pe[off], &v, 2); };
  auto w32 = [&](size_t off, u32 v) { memcpy(&pe[off], &v, 4); };
  auto w64 = [&](size_t off, u64 v) { memcpy(&pe[off], &v, 8); };
  pe[0] = 'M'; pe[1] = 'Z';
  w32(0x3C, 0x80);
  w32(0x80, 0x00004550u);               // PE\0\0
  w16(0x84, 0x8664u);                   // AMD64
  w16(0x86, 2);                         // 2 sections (.text, .reloc)
  w16(0x94, 0xF0);                      // SizeOfOptionalHeader = 240
  w16(0x96, 0x2022u);                   // EXECUTABLE_IMAGE | LARGE_ADDRESS_AWARE | DLL
  w16(0x98, 0x020Bu);                   // PE32+
  w32(0xA8, 0x1000u);                   // AddressOfEntryPoint
  w64(0xB0, 0x180000000ULL);            // ImageBase
  w32(0xB8, 0x1000u);                   // SectionAlignment
  w32(0xBC, 0x200u);                    // FileAlignment
  w32(0xD0, 0x3000u);                   // SizeOfImage
  w32(0xD4, 0x200u);                    // SizeOfHeaders
  w16(0xDC, 2u);                        // WINDOWS_GUI
  w16(0xDE, 0x0040u);                   // IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE
  w32(0xEC, 16u);                       // NumberOfRvaAndSizes
  // DataDirectory[5] BASERELOC at 0x108 + 5*8 = 0x130
  w32(0x130, 0x2000u);
  w32(0x134, 12u);
  // Section 0: .text at 0x188
  memcpy(&pe[0x188], ".text\0\0\0", 8);
  w32(0x190, 0x100u);                   // VirtualSize
  w32(0x194, 0x1000u);                  // VirtualAddress
  w32(0x198, 0x200u);                   // SizeOfRawData
  w32(0x19C, 0x200u);                   // PointerToRawData
  w32(0x1AC, 0x60000020u);              // CODE | EXECUTE | READ
  // Section 1: .reloc at 0x1B0
  memcpy(&pe[0x1B0], ".reloc\0\0", 8);
  w32(0x1B8, 12u);                      // VirtualSize
  w32(0x1BC, 0x2000u);                  // VirtualAddress
  w32(0x1C0, 0x200u);                   // SizeOfRawData
  w32(0x1C4, 0x400u);                   // PointerToRawData
  w32(0x1D4, 0x42000040u);              // INITIALIZED_DATA | DISCARDABLE | READ
  // mov eax, 1; ret
  static const u8 k_entry[6] = {0xB8, 1, 0, 0, 0, 0xC3};
  memcpy(&pe[0x200], k_entry, sizeof(k_entry));
  // Valid minimal reloc block (2 ABSOLUTE no-ops) at 0x400
  w32(0x400, 0x1000u);
  w32(0x404, 12u);
  return pe;
}

// ---------------------------------------------------------------------------
// Wave 2F: DXGI ABI fidelity — adapter/factory/output version-aware QI.
// Regression pins for the three live-tree gaps found by audit:
//   (a) adp_qi accepted every IID blindly (silent lie -> contract breach)
//   (b) Factory7 QI succeeded but the vtbl stopped at slot 29 (OOB call)
//   (c) Output object stopped at IDXGIOutput1 (SupportsOverlays/GetDesc1 gone)
// ---------------------------------------------------------------------------
void test_wave2f_dxgi_abi() {
  trt_gpu_hooks empty_hooks{};
  trt_gpu_set_hooks(&empty_hooks);

  using qi_fn = int (TRT_GUEST_ABI *)(void*, const void*, void**);
  using rel_fn = u32 (TRT_GUEST_ABI *)(void*);
  using enum1_fn = int (TRT_GUEST_ABI *)(void*, u32, void**);

  const raw_guid iid_unk = {0x00000000u, 0x0000, 0x0000, {0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
  const raw_guid iid_fac7 = {0xA4966EEDu, 0x76DB, 0x44DA, {0x84,0xC1,0xEE,0x9A,0x7A,0xFB,0x20,0xA8}};
  const raw_guid iid_adp1 = {0x29038F61u, 0x3839, 0x4626, {0x91,0xFD,0x08,0x68,0x79,0x01,0x1A,0x05}};
  const raw_guid iid_adp4 = {0x3C8D99D1u, 0x4FBF, 0x4181, {0xA8,0x2C,0xAF,0x66,0xBF,0x7B,0xD2,0x4E}};
  const raw_guid iid_garbage = {0xDEADBEEFu, 0x0000, 0x0000, {0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08}};
  const raw_guid iid_out6 = {0x068346E8u, 0xAAEC, 0x4B84, {0xAD,0xD7,0x13,0x7F,0x51,0x3F,0x77,0xA1}};
  const raw_guid iid_out2 = {0x595E39D1u, 0x2724, 0x4663, {0x99,0xB1,0xDA,0x96,0x9D,0xE2,0x83,0x64}};

  // (b) Factory7 QI + the two new tail methods callable, in-ABI
  void* fac = nullptr;
  CHECK(trt_CreateDXGIFactory2(0, (void*)&iid_fac7, &fac) == 0 && fac != nullptr,
        "CreateDXGIFactory2(IID_IDXGIFactory7) succeeds");
  using reg_fn = int (TRT_GUEST_ABI *)(void*, void*, u32*);
  using unreg_fn = int (TRT_GUEST_ABI *)(void*, u32);
  u32 cookie = 0;
  CHECK(fac && slot<reg_fn>(fac, 30)(fac, nullptr, &cookie) == 0 && cookie != 0,
        "IDXGIFactory7::RegisterAdaptersChangedEvent(slot 30) S_OK with cookie");
  CHECK(fac && slot<unreg_fn>(fac, 31)(fac, cookie) == 0,
        "IDXGIFactory7::UnregisterAdaptersChangedEvent(slot 31) S_OK");

  // (a) adapter QI is version-aware: full chain accepted, garbage rejected
  void* adapter = nullptr;
  CHECK(fac && slot<enum1_fn>(fac, 12)(fac, 0, &adapter) == 0 && adapter != nullptr,
        "IDXGIFactory1::EnumAdapters1(0) returns offline adapter");
  if (adapter) {
    CHECK(slot<qi_fn>(adapter, 0)(adapter, &iid_adp1, &adapter) == 0,
          "adapter QI(IID_IDXGIAdapter1) succeeds");
    CHECK(slot<qi_fn>(adapter, 0)(adapter, &iid_adp4, &adapter) == 0,
          "adapter QI(IID_IDXGIAdapter4) succeeds");
    void* nope = (void*)(uintptr_t)0x1;
    CHECK(slot<qi_fn>(adapter, 0)(adapter, &iid_garbage, &nope) == (int)0x80004002u &&
              nope == nullptr,
          "adapter QI(random IID) fails E_NOINTERFACE (fail-visible, not silent)");
    // adapter vtbl covers IDXGIAdapter4::GetDesc3 at slot 18
    using desc3_fn = int (TRT_GUEST_ABI *)(void*, void*);
    u8 desc[328];
    CHECK(slot<desc3_fn>(adapter, 18)(adapter, desc) == 0,
          "IDXGIAdapter4::GetDesc3(slot 18) callable");
    slot<rel_fn>(adapter, 2)(adapter);
  }

  // (c) output QI covers the Output2..Output6 chain + GetDesc1 callable
  using enum_out_fn = int (TRT_GUEST_ABI *)(void*, u32, void**);
  if (fac) {
    // fetch a fresh adapter and enumerate its output
    void* adp2 = nullptr;
    CHECK(slot<enum1_fn>(fac, 12)(fac, 0, &adp2) == 0 && adp2 != nullptr,
          "fresh adapter for output walk");
    if (adp2) {
      void* out = nullptr;
      CHECK(slot<enum_out_fn>(adp2, 7)(adp2, 0, &out) == 0 && out != nullptr,
            "IDXGIAdapter::EnumOutputs(0) returns offline output");
      if (out) {
        CHECK(slot<qi_fn>(out, 0)(out, &iid_out2, &out) == 0,
              "output QI(IID_IDXGIOutput2) succeeds (SupportsOverlays served)");
        CHECK(slot<qi_fn>(out, 0)(out, &iid_out6, &out) == 0,
              "output QI(IID_IDXGIOutput6) succeeds (GetDesc1/HDR served)");
        void* nope = (void*)(uintptr_t)0x1;
        CHECK(slot<qi_fn>(out, 0)(out, &iid_garbage, &nope) == (int)0x80004002u &&
                  nope == nullptr,
              "output QI(random IID) fails E_NOINTERFACE");
        // SupportsOverlays at slot 23 (IDXGIOutput2)
        using overlays_fn = int (TRT_GUEST_ABI *)(void*);
        CHECK(slot<overlays_fn>(out, 23)(out) == 0,
              "IDXGIOutput2::SupportsOverlays(slot 23) callable");
        // GetDesc1 at slot 27: DESC prefix 96 bytes, BitsPerColor at +96,
        // ColorSpace +100, 8 floats +104..+135, MinLum +136, MaxLum +140.
        using desc1_fn = int (TRT_GUEST_ABI *)(void*, void*);
        u8 d1[160];
        CHECK(slot<desc1_fn>(out, 27)(out, d1) == 0 &&
                  *(u32*)(d1 + 96) == 10u && *(float*)(d1 + 140) > 0.0f,
              "IDXGIOutput6::GetDesc1(slot 27) fills BitsPerColor=10 + MaxLuminance");
        slot<rel_fn>(out, 2)(out);
      }
      slot<rel_fn>(adp2, 2)(adp2);
    }
  }

  // IUnknown always works on the factory
  void* unk = nullptr;
  CHECK(fac && slot<qi_fn>(fac, 0)(fac, &iid_unk, &unk) == 0 && unk == fac,
        "factory QI(IID_IUnknown) succeeds");
  if (fac) slot<rel_fn>(fac, 2)(fac);
}

void test_wave2e_sys_net_d3d9() {
  // Case-insensitive DLL deduplication
  char dll_tpl[] = "/tmp/trt_w2_dll_XXXXXX";
  char* dll_dir = mkdtemp(dll_tpl);
  if (dll_dir) {
    std::vector<u8> pe = build_min_pe64_dll();
    w2_write_file(std::string(dll_dir) + "/CaseTest.dll", pe.data(), pe.size());
    trt_images_reset();
    trt_backend be{&linux_alloc, &linux_protect, &linux_release, &linux_log, nullptr};
    trt_runtime_init(&be, dll_dir);
    using LoadLibA_t = void* (TRT_STDCALL *)(const char*);
    using FreeLib_t = int (TRT_STDCALL *)(void*);
    auto pLoadA = (LoadLibA_t)trt_resolve_module(nullptr, "kernel32.dll", "LoadLibraryA", 0);
    auto pFreeL = (FreeLib_t)trt_resolve_module(nullptr, "kernel32.dll", "FreeLibrary", 0);
    void* h1 = pLoadA ? pLoadA("CaseTest.dll") : nullptr;
    void* h2 = pLoadA ? pLoadA("CASETEST.DLL") : nullptr;
    void* h3 = pLoadA ? pLoadA("casetest") : nullptr;
    CHECK(h1 != nullptr && h1 == h2 && h2 == h3 && trt_images_count() == 1,
          "Case-insensitive LoadLibraryA deduplicates CaseTest.dll / CASETEST.DLL");
    if (pFreeL && h1) { pFreeL(h1); pFreeL(h2); pFreeL(h3); }
    trt_images_reset();
    w2_rm_tree(dll_dir);
  }

  // mswsock + WSAIoctl(SIO_GET_EXTENSION_FUNCTION_POINTER) for all 9 GUIDs
  CHECK(trt_module_route("mswsock.dll") == TRT_ROUTE_BUILTIN &&
            trt_module_query("mswsock.dll", "ConnectEx") == 1 &&
            trt_module_query("mswsock.dll", "AcceptEx") == 1 &&
            trt_module_query("mswsock.dll", "TransmitPackets") == 1,
        "mswsock.dll registered BUILTIN with extension functions");
  using WSAStartup_t = i32 (TRT_GUEST_ABI *)(u16, void*);
  using WSACleanup_t = i32 (TRT_GUEST_ABI *)(void);
  using Socket_t = u64 (TRT_GUEST_ABI *)(i32, i32, i32);
  using CloseSock_t = i32 (TRT_GUEST_ABI *)(u64);
  using WSAIoctl_t = i32 (TRT_GUEST_ABI *)(u64, u32, const void*, u32, void*, u32, u32*, void*, void*);

  auto pWSAStart = (WSAStartup_t)trt_ws2_resolve("WSAStartup");
  auto pWSAClean = (WSACleanup_t)trt_ws2_resolve("WSACleanup");
  auto pSocket = (Socket_t)trt_ws2_resolve("socket");
  auto pCloseS = (CloseSock_t)trt_ws2_resolve("closesocket");
  auto pWSAIoctl = (WSAIoctl_t)trt_ws2_resolve("WSAIoctl");

  u8 wsa_data[512] = {};
  u64 sock = (u64)-1;
  int ioctl_ok = 0;
  if (pWSAStart && pWSAStart(0x0202u, wsa_data) == 0 && pSocket && pWSAIoctl) {
    sock = pSocket(2 /*AF_INET*/, 1 /*SOCK_STREAM*/, 6 /*IPPROTO_TCP*/);
    static const raw_guid k_ext_guids[9] = {
        {0x25a207b9u, 0xddf3u, 0x4660u, {0x8e,0xe9,0x76,0xe5,0x8c,0x74,0x06,0x3e}}, // ConnectEx
        {0xb5367df1u, 0xcbacu, 0x11cfu, {0x95,0xca,0x00,0x80,0x5f,0x48,0xa1,0x92}}, // AcceptEx
        {0xb5367df2u, 0xcbacu, 0x11cfu, {0x95,0xca,0x00,0x80,0x5f,0x48,0xa1,0x92}}, // GetAcceptExSockaddrs
        {0x7fda2e11u, 0x8630u, 0x436fu, {0xa0,0x31,0xf5,0x36,0xa6,0xee,0xc1,0x57}}, // DisconnectEx
        {0xb5367df0u, 0xcbacu, 0x11cfu, {0x95,0xca,0x00,0x80,0x5f,0x48,0xa1,0x92}}, // TransmitFile
        {0xd9689da0u, 0x1f90u, 0x11d3u, {0x99,0x71,0x00,0xc0,0x4f,0x68,0xc8,0x76}}, // TransmitPackets
        {0xf689d7c8u, 0x6f1fu, 0x436bu, {0x8a,0x53,0xe5,0x4f,0xe3,0x51,0xc3,0x22}}, // WSARecvMsg
        {0xa441e712u, 0x754fu, 0x43cau, {0x84,0xa7,0x0d,0xee,0x44,0xcf,0x60,0x6d}}, // WSASendMsg
        {0x18c76f85u, 0xdc66u, 0x4964u, {0x97,0x2e,0x23,0xc2,0x72,0x38,0x31,0x2b}}, // WSAPoll
    };
    for (int i = 0; i < 9 && sock != (u64)-1; ++i) {
      void* fn_out = nullptr;
      u32 got = 0;
      if (pWSAIoctl(sock, 0xC8000006u, &k_ext_guids[i], 16, &fn_out, sizeof(fn_out),
                    &got, nullptr, nullptr) == 0 &&
          fn_out != nullptr && got == sizeof(fn_out)) {
        ++ioctl_ok;
      }
    }
    if (sock != (u64)-1 && pCloseS) pCloseS(sock);
    if (pWSAClean) pWSAClean();
  }
  CHECK(ioctl_ok == 9, "WSAIoctl SIO_GET_EXTENSION_FUNCTION_POINTER resolves all 9 WSAID GUIDs");

  // normaliz.dll RFC 3492 Punycode
  using IdnFn_t = i32 (TRT_GUEST_ABI *)(u32, const u16*, i32, u16*, i32);
  auto pIdnAsc = (IdnFn_t)trt_resolve_module(nullptr, "normaliz.dll", "IdnToAscii", 0);
  auto pIdnUni = (IdnFn_t)trt_resolve_module(nullptr, "normaliz.dll", "IdnToUnicode", 0);
  static const u16 k_idn_src[] = {'b', 0x00FC, 'c', 'h', 'e', 'r', '.', 'd', 'e', 0};
  u16 ascii_dom[128] = {};
  u16 uni_dom[128] = {};
  i32 n1 = pIdnAsc ? pIdnAsc(0, k_idn_src, -1, ascii_dom, 128) : 0;
  i32 n2 = (n1 > 0 && pIdnUni) ? pIdnUni(0, ascii_dom, -1, uni_dom, 128) : 0;
  CHECK(n1 > 0 && u16_to_str(ascii_dom) == "xn--bcher-kva.de" &&
            n2 > 0 && memcmp(uni_dom, k_idn_src, sizeof(k_idn_src)) == 0,
        "normaliz.dll IdnToAscii/IdnToUnicode roundtrips RFC 3492 Punycode");

  // sechost / ncrypt / usp10 / cryptui / credui / dxcore / directxmath
  using OpenSCMW_t = void* (TRT_GUEST_ABI *)(const u16*, const u16*, u32);
  using StrToSidW_t = i32 (TRT_GUEST_ABI *)(const u16*, void**);
  using SidToStrW_t = i32 (TRT_GUEST_ABI *)(const void*, u16**);
  using LocalFree_t = void* (TRT_STDCALL *)(void*);
  auto pOpenSCM = (OpenSCMW_t)trt_resolve_module(nullptr, "sechost.dll", "OpenSCManagerW", 0);
  auto pS2Sid = (StrToSidW_t)trt_resolve_module(nullptr, "sechost.dll", "ConvertStringSidToSidW", 0);
  auto pSid2S = (SidToStrW_t)trt_resolve_module(nullptr, "sechost.dll", "ConvertSidToStringSidW", 0);
  auto pLFree = (LocalFree_t)trt_resolve_module(nullptr, "kernel32.dll", "LocalFree", 0);
  void* sid_ptr = nullptr;
  u16* sid_str = nullptr;
  auto w_sid = u16_lit("S-1-5-18");
  if (pS2Sid && pS2Sid(w_sid.data(), &sid_ptr) && sid_ptr && pSid2S) {
    pSid2S(sid_ptr, &sid_str);
  }
  CHECK(pOpenSCM && pOpenSCM(nullptr, nullptr, 1) == nullptr &&
            sid_str && u16_to_str(sid_str) == "S-1-5-18",
        "sechost.dll OpenSCManagerW fails named + SID string/binary roundtrips");
  if (sid_ptr && pLFree) pLFree(sid_ptr);
  if (sid_str && pLFree) pLFree(sid_str);

  using NCOpen_t = i32 (TRT_GUEST_ABI *)(u64*, const u16*, u32);
  using NCEnum_t = i32 (TRT_GUEST_ABI *)(u32*, void**, u32);
  using NCFreeBuf_t = i32 (TRT_GUEST_ABI *)(void*);
  using NCFreeObj_t = i32 (TRT_GUEST_ABI *)(u64);
  auto pNCOpen = (NCOpen_t)trt_resolve_module(nullptr, "ncrypt.dll", "NCryptOpenStorageProvider", 0);
  auto pNCEnum = (NCEnum_t)trt_resolve_module(nullptr, "ncrypt.dll", "NCryptEnumStorageProviders", 0);
  auto pNCFreeB = (NCFreeBuf_t)trt_resolve_module(nullptr, "ncrypt.dll", "NCryptFreeBuffer", 0);
  auto pNCFreeO = (NCFreeObj_t)trt_resolve_module(nullptr, "ncrypt.dll", "NCryptFreeObject", 0);
  u64 nc_h = 0;
  u32 nc_cnt = 0;
  void* nc_list = nullptr;
  bool nc_ok = pNCOpen && pNCOpen(&nc_h, nullptr, 0) == 0 && nc_h != 0 &&
               pNCEnum && pNCEnum(&nc_cnt, &nc_list, 0) == 0 && nc_cnt >= 1 && nc_list != nullptr;
  if (nc_list && pNCFreeB) pNCFreeB(nc_list);
  if (nc_h && pNCFreeO) pNCFreeO(nc_h);
  CHECK(nc_ok, "ncrypt.dll opens and enumerates storage provider cleanly");

  // usp10 shaping + ScriptStringAnalyse pipeline
  using SItem_t = i32 (TRT_GUEST_ABI *)(const u16*, i32, i32, const void*, const void*, void*, i32*);
  using SShape_t = i32 (TRT_GUEST_ABI *)(void*, void**, const u16*, i32, i32, void*, u16*, u16*, void*, i32*);
  using SPlace_t = i32 (TRT_GUEST_ABI *)(void*, void**, const u16*, i32, const void*, void*, i32*, i32*, void*);
  using SSAna_t = i32 (TRT_GUEST_ABI *)(void*, const void*, i32, i32, i32, u32, i32, void*, void*, const i32*, void*, const u8*, void**);
  using SSOut_t = const i32* (TRT_GUEST_ABI *)(void*);
  using SSFree_t = i32 (TRT_GUEST_ABI *)(void**);
  auto pSItem = (SItem_t)trt_resolve_module(nullptr, "usp10.dll", "ScriptItemize", 0);
  auto pSShape = (SShape_t)trt_resolve_module(nullptr, "usp10.dll", "ScriptShape", 0);
  auto pSPlace = (SPlace_t)trt_resolve_module(nullptr, "usp10.dll", "ScriptPlace", 0);
  auto pSSA = (SSAna_t)trt_resolve_module(nullptr, "usp10.dll", "ScriptStringAnalyse", 0);
  auto pSSO = (SSOut_t)trt_resolve_module(nullptr, "usp10.dll", "ScriptString_pcOutChars", 0);
  auto pSSF = (SSFree_t)trt_resolve_module(nullptr, "usp10.dll", "ScriptStringFree", 0);
  struct { i32 pos; u32 a; u32 s; } items[8] = {};
  i32 n_items = 0, n_glyphs = 0;
  u16 glyphs[16] = {}, logc[16] = {};
  u16 visattr[16] = {};
  i32 adv[16] = {};
  i32 goff[32] = {};
  u8 abc[12] = {};
  void* scache = nullptr;
  auto w_hi = u16_lit("Hello");
  bool usp_ok = false;
  if (pSItem && pSShape && pSPlace &&
      pSItem(w_hi.data(), 5, 8, nullptr, nullptr, items, &n_items) == 0 && n_items == 1 &&
      pSShape(nullptr, &scache, w_hi.data(), 5, 16, &items[0].a, glyphs, logc, visattr, &n_glyphs) == 0 &&
      n_glyphs == 5 &&
      pSPlace(nullptr, &scache, glyphs, 5, visattr, &items[0].a, adv, goff, abc) == 0 &&
      adv[0] > 0) {
    usp_ok = true;
  }
  void* ssa = nullptr;
  if (pSSA && pSSA(nullptr, w_hi.data(), 5, 0, -1, 0, 0, nullptr, nullptr, nullptr, nullptr, nullptr, &ssa) == 0 && ssa) {
    const i32* out_c = pSSO ? pSSO(ssa) : nullptr;
    if (!out_c || *out_c != 5) usp_ok = false;
    if (pSSF) pSSF(&ssa);
  } else {
    usp_ok = false;
  }
  CHECK(usp_ok, "usp10.dll ScriptItemize/Shape/Place and ScriptStringAnalyse succeed");

  using CredParseW_t = u32 (TRT_GUEST_ABI *)(const u16*, u16*, u32, u16*, u32);
  using CryptUIDlg_t = i32 (TRT_GUEST_ABI *)(void*);
  using DXCFactory_t = i32 (TRT_GUEST_ABI *)(const void*, void**);
  using DXMVerify_t = u8 (TRT_GUEST_ABI *)(void);
  auto pCredParse = (CredParseW_t)trt_resolve_module(nullptr, "credui.dll", "CredUIParseUserNameW", 0);
  auto pCryptUI = (CryptUIDlg_t)trt_resolve_module(nullptr, "cryptui.dll", "CryptUIDlgViewContext", 0);
  auto pDXCCreate = (DXCFactory_t)trt_resolve_module(nullptr, "dxcore.dll", "DXCoreCreateAdapterFactory", 0);
  auto pDXM = (DXMVerify_t)trt_resolve_module(nullptr, "directxmath.dll", "XMVerifyCPUSupport", 0);

  u16 dom_buf[64] = {}, usr_buf[64] = {};
  auto w_cred = u16_lit("CORP\\alice");
  CHECK(pCredParse && pCredParse(w_cred.data(), usr_buf, 64, dom_buf, 64) == 0 &&
            u16_to_str(dom_buf) == "CORP" && u16_to_str(usr_buf) == "alice",
        "credui.dll CredUIParseUserNameW splits DOMAIN\\user");
  CHECK(pCryptUI && trt_module_route("cryptui.dll") == TRT_ROUTE_BUILTIN &&
            pCryptUI(nullptr) == 0,
        "cryptui.dll registered BUILTIN and CryptUIDlgViewContext cancels safely");
  CHECK(pDXM && pDXM() == 1, "directxmath.dll XMVerifyCPUSupport returns TRUE");

  void* dxc_fac = nullptr;
  static const raw_guid k_iid_dxc_fac =
      {0x78ee5945u, 0xc36eu, 0x4b13u, {0xa6,0x69,0x00,0x5d,0xd1,0x1c,0x0f,0x06}};
  static const raw_guid k_iid_dxc_list =
      {0x526c7776u, 0x40e9u, 0x459bu, {0xb7,0x11,0xf3,0x2a,0xd7,0x6d,0xfc,0x28}};
  static const raw_guid k_iid_dxc_adp =
      {0xf0db4c7fu, 0xfe5au, 0x42a2u, {0xbd,0x62,0xf2,0xa6,0xcf,0x6f,0xc8,0x3e}};
  static const raw_guid k_dxc_attr_d3d12 =
      {0x0c9ece4du, 0x2f6eu, 0x4f01u, {0x8c,0x96,0xe8,0x9e,0x33,0x1b,0x47,0xb1}};
  bool dxc_ok = false;
  if (pDXCCreate && pDXCCreate(&k_iid_dxc_fac, &dxc_fac) == 0 && dxc_fac) {
    void* dxc_list = nullptr;
    using CreateList_t = i32 (TRT_GUEST_ABI *)(void*, u32, const void*, const void*, void**);
    using GetCnt_t = u32 (TRT_GUEST_ABI *)(void*);
    using GetAdp_t = i32 (TRT_GUEST_ABI *)(void*, u32, const void*, void**);
    using GetProp_t = i32 (TRT_GUEST_ABI *)(void*, u32, u64, void*);
    if (slot<CreateList_t>(dxc_fac, 3)(dxc_fac, 1, &k_dxc_attr_d3d12, &k_iid_dxc_list, &dxc_list) == 0 &&
        dxc_list && slot<GetCnt_t>(dxc_list, 4)(dxc_list) >= 1) {
      void* dxc_adp = nullptr;
      if (slot<GetAdp_t>(dxc_list, 3)(dxc_list, 0, &k_iid_dxc_adp, &dxc_adp) == 0 && dxc_adp) {
        u32 hw[2] = {};
        if (slot<GetProp_t>(dxc_adp, 6)(dxc_adp, 0 /*InstanceLuid*/, 8, hw) == 0) dxc_ok = true;
        slot<u32 (TRT_GUEST_ABI *)(void*)>(dxc_adp, 2)(dxc_adp);
      }
      slot<u32 (TRT_GUEST_ABI *)(void*)>(dxc_list, 2)(dxc_list);
    }
    slot<u32 (TRT_GUEST_ABI *)(void*)>(dxc_fac, 2)(dxc_fac);
  }
  CHECK(dxc_ok, "dxcore.dll DXCoreCreateAdapterFactory enumerates RDNA2 adapter");

  // D3D9: MojoShader HLSL, CreateVertexDeclaration, StateBlock, Constants, Cube/Volume/Offscreen surfaces
  struct d3d9_cap {
    bool got_shader = false;
    std::string hlsl;
  } d9cap;
  d3d9_hooks d9h{};
  d9h.present = [](void*) {};
  d9h.create_shader = [](u32 is_ps, u32 sm_maj, u32 sm_min, const char* hlsl,
                         u32 hlsl_len, const void*, void* u) -> u32 {
    auto* c = (d3d9_cap*)u;
    if (!is_ps && sm_maj == 2 && sm_min == 0 && hlsl_len > 0 && hlsl && hlsl[0]) {
      c->got_shader = true;
      c->hlsl = hlsl;
    }
    return 77u;
  };
  d9h.user = &d9cap;
  trt_d3d9_set_hooks(&d9h);

  using D3DCreate9_t = void* (TRT_GUEST_ABI *)(u32);
  auto pD3D9 = (D3DCreate9_t)trt_resolve_module(nullptr, "d3d9.dll", "Direct3DCreate9", 0);
  void* d3d = pD3D9 ? pD3D9(32) : nullptr;
  void* dev = nullptr;
  u32 pp[16] = {1280, 720, 21, 1};
  if (d3d) {
    using CreateDev_t = i32 (TRT_GUEST_ABI *)(void*, u32, u32, void*, u32, void*, void**);
    slot<CreateDev_t>(d3d, 16)(d3d, 0, 1, nullptr, 0x40, pp, &dev);
  }
  CHECK(dev != nullptr, "Direct3DCreate9 + CreateDevice succeeds for extended wave2 checks");
  if (dev) {
    using Rel_t = u32 (TRT_GUEST_ABI *)(void*);
    // CreateVertexShader + MojoShader HLSL + GetFunction
    static const u32 k_vs20[8] = {
        0xFFFE0200u, 0x0200001Fu, 0x80000000u, 0x900F0000u,
        0x02000001u, 0xC00F0000u, 0x90E40000u, 0x0000FFFFu};
    void* vs = nullptr;
    u32 got_vs_len = 0;
    u32 got_vs[8] = {};
    using CreateShader_t = i32 (TRT_GUEST_ABI *)(void*, const u32*, void**);
    using GetFunc_t = i32 (TRT_GUEST_ABI *)(void*, void*, u32*);
    slot<CreateShader_t>(dev, 91)(dev, k_vs20, &vs);
    if (vs) {
      slot<GetFunc_t>(vs, 4)(vs, nullptr, &got_vs_len);
      if (got_vs_len == 32) slot<GetFunc_t>(vs, 4)(vs, got_vs, &got_vs_len);
      slot<Rel_t>(vs, 2)(vs);
    }
    CHECK(d9cap.got_shader && d9cap.hlsl.find("main") != std::string::npos &&
              memcmp(got_vs, k_vs20, 32) == 0,
          "D3D9 CreateVertexShader emits MojoShader HLSL + GetFunction roundtrips bytecode");

    // VertexDeclaration + GetDeclaration
    static const d3d9_vert_elem k_elems[2] = {
        {0, 0, 2 /*FLOAT3*/, 0, 0 /*POSITION*/, 0},
        {0xFF, 0, 17 /*UNUSED*/, 0, 0, 0},
    };
    void* decl = nullptr;
    d3d9_vert_elem dec_out[4] = {};
    u32 dec_cnt = 0;
    using CreateDecl_t = i32 (TRT_GUEST_ABI *)(void*, const void*, void**);
    using GetDecl_t = i32 (TRT_GUEST_ABI *)(void*, void*, u32*);
    slot<CreateDecl_t>(dev, 86)(dev, k_elems, &decl);
    if (decl) {
      slot<GetDecl_t>(decl, 4)(decl, dec_out, &dec_cnt);
      slot<Rel_t>(decl, 2)(decl);
    }
    CHECK(dec_cnt == 2 && dec_out[0].type == 2 && dec_out[1].stream == 0xFF,
          "D3D9 CreateVertexDeclaration + GetDeclaration preserves vertex elements");

    // StateBlock (Begin/End + CreateStateBlock/Capture/Apply) restores render state + constants
    using SetRS_t = i32 (TRT_GUEST_ABI *)(void*, u32, u32);
    using GetRS_t = i32 (TRT_GUEST_ABI *)(void*, u32, u32*);
    using CreateSB_t = i32 (TRT_GUEST_ABI *)(void*, u32, void**);
    using BeginSB_t = i32 (TRT_GUEST_ABI *)(void*);
    using EndSB_t = i32 (TRT_GUEST_ABI *)(void*, void**);
    using CapSB_t = i32 (TRT_GUEST_ABI *)(void*);
    using ApplySB_t = i32 (TRT_GUEST_ABI *)(void*);
    using SetConstF_t = i32 (TRT_GUEST_ABI *)(void*, u32, const float*, u32);
    using GetConstF_t = i32 (TRT_GUEST_ABI *)(void*, u32, float*, u32);
    using SetConstI_t = i32 (TRT_GUEST_ABI *)(void*, u32, const int*, u32);
    using GetConstI_t = i32 (TRT_GUEST_ABI *)(void*, u32, int*, u32);
    using SetConstB_t = i32 (TRT_GUEST_ABI *)(void*, u32, const int*, u32);
    using GetConstB_t = i32 (TRT_GUEST_ABI *)(void*, u32, int*, u32);

    const float vcf_orig[4] = {1.f, 2.f, 3.f, 4.f};
    const float pcf_orig[4] = {5.f, 6.f, 7.f, 8.f};
    void* sb_rec = nullptr;
    slot<BeginSB_t>(dev, 60)(dev);
    slot<SetRS_t>(dev, 57)(dev, 7 /*ZENABLE*/, 1);
    slot<SetConstF_t>(dev, 94)(dev, 0, vcf_orig, 1);
    slot<SetConstF_t>(dev, 109)(dev, 0, pcf_orig, 1);
    slot<EndSB_t>(dev, 61)(dev, &sb_rec);

    void* sb_all = nullptr;
    slot<CreateSB_t>(dev, 59)(dev, 1 /*D3DSBT_ALL*/, &sb_all);
    slot<SetRS_t>(dev, 57)(dev, 7, 1);
    slot<SetConstF_t>(dev, 94)(dev, 0, vcf_orig, 1);
    slot<SetConstF_t>(dev, 109)(dev, 0, pcf_orig, 1);
    if (sb_all) slot<CapSB_t>(sb_all, 4)(sb_all);

    slot<SetRS_t>(dev, 57)(dev, 7, 0);
    const float vcf_dirty[4] = {9.f, 9.f, 9.f, 9.f};
    slot<SetConstF_t>(dev, 94)(dev, 0, vcf_dirty, 1);
    if (sb_rec) {
      slot<ApplySB_t>(sb_rec, 5)(sb_rec);
      slot<Rel_t>(sb_rec, 2)(sb_rec);
    }
    if (sb_all) {
      slot<SetRS_t>(dev, 57)(dev, 7, 0);
      slot<ApplySB_t>(sb_all, 5)(sb_all);
      slot<Rel_t>(sb_all, 2)(sb_all);
    }
    u32 rs_val = 99;
    float vcf_got[4] = {}, pcf_got[4] = {};
    slot<GetRS_t>(dev, 58)(dev, 7, &rs_val);
    slot<GetConstF_t>(dev, 95)(dev, 0, vcf_got, 1);
    slot<GetConstF_t>(dev, 110)(dev, 0, pcf_got, 1);

    const int ci[4] = {11, 22, 33, 44};
    const int cb[1] = {1};
    int ci_got[4] = {}, cb_got[1] = {};
    slot<SetConstI_t>(dev, 96)(dev, 1, ci, 1);
    slot<GetConstI_t>(dev, 97)(dev, 1, ci_got, 1);
    slot<SetConstB_t>(dev, 113)(dev, 2, cb, 1);
    slot<GetConstB_t>(dev, 114)(dev, 2, cb_got, 1);
    CHECK(rs_val == 1 && vcf_got[0] == 1.f && pcf_got[3] == 8.f &&
              ci_got[2] == 33 && cb_got[0] == 1,
          "D3D9 StateBlock + VS/PS Constant F/I/B roundtrip accurately");

    // CubeTexture + VolumeTexture + OffscreenPlainSurface + Texture GetSurfaceLevel/ColorFill/UpdateSurface
    using CreateTex_t = i32 (TRT_GUEST_ABI *)(void*, u32, u32, u32, u32, u32, u32, void**, void*);
    using CreateVol_t = i32 (TRT_GUEST_ABI *)(void*, u32, u32, u32, u32, u32, u32, u32, void**, void*);
    using CreateCube_t = i32 (TRT_GUEST_ABI *)(void*, u32, u32, u32, u32, u32, void**, void*);
    using CreateOff_t = i32 (TRT_GUEST_ABI *)(void*, u32, u32, u32, u32, void**, void*);
    using ColorFill_t = i32 (TRT_GUEST_ABI *)(void*, void*, const void*, u32);
    using UpdateSurf_t = i32 (TRT_GUEST_ABI *)(void*, void*, const void*, void*, const void*);
    using UpdateTex_t = i32 (TRT_GUEST_ABI *)(void*, void*, void*);

    struct locked_rect { i32 pitch; i32 pad; void* bits; };
    struct locked_box { i32 rp; i32 sp; void* bits; };

    void *ctex = nullptr, *vtex = nullptr, *tex = nullptr, *tex2 = nullptr;
    void *off1 = nullptr, *off2 = nullptr;
    slot<CreateCube_t>(dev, 25)(dev, 16, 1, 0, 21, 1, &ctex, nullptr);
    slot<CreateVol_t>(dev, 24)(dev, 8, 8, 4, 1, 0, 21, 1, &vtex, nullptr);
    slot<CreateTex_t>(dev, 23)(dev, 16, 16, 1, 0, 21, 1, &tex, nullptr);
    slot<CreateTex_t>(dev, 23)(dev, 16, 16, 1, 0, 21, 0, &tex2, nullptr);
    slot<CreateOff_t>(dev, 36)(dev, 4, 4, 21, 3, &off1, nullptr);
    slot<CreateOff_t>(dev, 36)(dev, 4, 4, 21, 3, &off2, nullptr);

    bool surf_ok = false;
    if (ctex && vtex && tex && tex2 && off1 && off2) {
      using GetCubeS_t = i32 (TRT_GUEST_ABI *)(void*, u32, u32, void**);
      using GetCont_t = i32 (TRT_GUEST_ABI *)(void*, const void*, void**);
      using GetVolL_t = i32 (TRT_GUEST_ABI *)(void*, u32, void**);
      using LockBox_t = i32 (TRT_GUEST_ABI *)(void*, void*, const void*, u32);
      using UnlockVol_t = i32 (TRT_GUEST_ABI *)(void*);
      using GetSurfL_t = i32 (TRT_GUEST_ABI *)(void*, u32, void**);
      using LockSurf_t = i32 (TRT_GUEST_ABI *)(void*, void*, const void*, u32);
      using UnlockSurf_t = i32 (TRT_GUEST_ABI *)(void*);
      using LockTex_t = i32 (TRT_GUEST_ABI *)(void*, u32, void*, const void*, u32);
      using UnlockTex_t = i32 (TRT_GUEST_ABI *)(void*, u32);

      void* csurf = nullptr;
      void* cont = nullptr;
      slot<GetCubeS_t>(ctex, 18)(ctex, 2, 0, &csurf);
      if (csurf) {
        slot<GetCont_t>(csurf, 11)(csurf, nullptr, &cont);
        if (cont) slot<Rel_t>(cont, 2)(cont);
        slot<Rel_t>(csurf, 2)(csurf);
      }

      void* vol = nullptr;
      locked_box lb{};
      slot<GetVolL_t>(vtex, 18)(vtex, 0, &vol);
      if (vol) {
        slot<LockBox_t>(vol, 9)(vol, &lb, nullptr, 0);
        slot<UnlockVol_t>(vol, 10)(vol);
        slot<Rel_t>(vol, 2)(vol);
      }

      void* tsurf = nullptr;
      slot<GetSurfL_t>(tex, 18)(tex, 0, &tsurf);
      locked_rect lr_off{};
      slot<ColorFill_t>(dev, 35)(dev, off1, nullptr, 0xAABBCCDDu);
      slot<UpdateSurf_t>(dev, 30)(dev, off1, nullptr, off2, nullptr);
      slot<LockSurf_t>(off2, 13)(off2, &lr_off, nullptr, 0);
      u32 px = lr_off.bits ? *(u32*)lr_off.bits : 0u;
      slot<UnlockSurf_t>(off2, 14)(off2);

      locked_rect lr_ts{};
      i32 lts = tsurf ? slot<LockSurf_t>(tsurf, 13)(tsurf, &lr_ts, nullptr, 0) : -1;
      if (lts == 0 && lr_ts.bits) *(u32*)lr_ts.bits = 0x11223344u;
      if (tsurf && lts == 0) slot<UnlockSurf_t>(tsurf, 14)(tsurf);
      if (tsurf) slot<Rel_t>(tsurf, 2)(tsurf);

      slot<UpdateTex_t>(dev, 31)(dev, tex, tex2);
      locked_rect lr_t2{};
      u32 tpx = 0;
      if (slot<LockTex_t>(tex2, 19)(tex2, 0, &lr_t2, nullptr, 0) == 0 && lr_t2.bits) {
        tpx = *(u32*)lr_t2.bits;
        slot<UnlockTex_t>(tex2, 20)(tex2, 0);
      }

      surf_ok = (cont == ctex && lb.bits != nullptr && lb.sp == 8 * 4 * 8 &&
                 px == 0xAABBCCDDu && lts == 0 && lr_ts.bits != nullptr &&
                 tpx == 0x11223344u);
    }
    if (ctex) slot<Rel_t>(ctex, 2)(ctex);
    if (vtex) slot<Rel_t>(vtex, 2)(vtex);
    if (tex) slot<Rel_t>(tex, 2)(tex);
    if (tex2) slot<Rel_t>(tex2, 2)(tex2);
    if (off1) slot<Rel_t>(off1, 2)(off1);
    if (off2) slot<Rel_t>(off2, 2)(off2);
    CHECK(surf_ok,
          "D3D9 CubeTexture/VolumeTexture/OffscreenPlainSurface ColorFill/UpdateSurface/GetContainer work");
    slot<Rel_t>(dev, 2)(dev);
  }
  if (d3d) slot<u32 (TRT_GUEST_ABI *)(void*)>(d3d, 2)(d3d);
  d3d9_hooks empty_d9{};
  trt_d3d9_set_hooks(&empty_d9);
}

}  // namespace

int main() {
  printf("tiramisu wave2 runtime verification suite\n");
  trt_backend be{&linux_alloc, &linux_protect, &linux_release, &linux_log, nullptr};
  trt_bridge_boot(&be);
  trt_bridge_register_kernel32();
  trt_bridge_register_user32();
  trt_bridge_register_ws2();
  trt_bridge_register_msvcrt();

  test_wave2a_d3d12();
  test_wave2b_mf();
  test_wave2c_vfs_cow();
  test_wave2d_vmem_jit();
  test_wave2e_sys_net_d3d9();
  test_wave2f_dxgi_abi();

  printf("\nRESULT: %s\n", g_failures ? "FAIL" : "ALL PASS");
  return g_failures ? 1 : 0;
}
