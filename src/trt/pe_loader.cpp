// tiramisu/src/trt/pe_loader.cpp
// Tiramisu Runtime PE64 loader core. Wine-style in-process mapping:
//   validate -> alloc -> headers+sections -> relocate -> bind imports -> TLS
// Zero RWX: everything maps RW; trt_seal_image() flips code to RX.
// Bounds-checked throughout: a malformed PE must fail with a named error,
// never crash the host process (the host IS the console app).
#include "trt.h"
#include "thunk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#  include <windows.h>
#else
#  include <pthread.h>
#endif
#include <atomic>
#include <mutex>
#include <vector>
#include <string>

#if defined(_WIN32)
namespace {
struct RecentImport {
  volatile u32 site;
  volatile u32 tid;
  volatile uintptr_t caller;
};
static_assert(sizeof(RecentImport) == 16);
struct ImportLocation { char module[72]; char function[96]; };
std::atomic<u32> g_recent_import_tick{0};
std::atomic<u32> g_import_sites_count{0};
RecentImport g_recent_imports[64]{};
ImportLocation g_import_sites[4096]{};

void* trace_guest_import(const trt_backend* be, const char* module,
                         const char* function, u16 ordinal, void* fn) {
  const u32 site_index = g_import_sites_count.fetch_add(1, std::memory_order_relaxed);
  if (site_index >= sizeof(g_import_sites) / sizeof(g_import_sites[0])) return fn;
  ImportLocation& where = g_import_sites[site_index];
  snprintf(where.module, sizeof(where.module), "%s", module);
  if (function) snprintf(where.function, sizeof(where.function), "%s", function);
  else snprintf(where.function, sizeof(where.function), "#%u", (unsigned)ordinal);
  // Transparent tail trampoline. Records the native TEB thread id and the
  // guest return address, preserves incoming registers and calls the target.
  constexpr u8 kTrace[] = {
      0x9c, 0x50, 0x41, 0x52, 0x41, 0x53, 0x48, 0xb8, 0x81, 0x70, 0x60, 0x50,
      0x40, 0x30, 0x20, 0x10, 0x41, 0xba, 0x01, 0x00, 0x00, 0x00, 0xf0, 0x44,
      0x0f, 0xc1, 0x10, 0x41, 0x83, 0xe2, 0x3f, 0x41, 0xc1, 0xe2, 0x04, 0x48,
      0xb8, 0x82, 0x70, 0x60, 0x50, 0x40, 0x30, 0x20, 0x10, 0x4c, 0x01, 0xd0,
      0xc7, 0x00, 0x83, 0x30, 0x20, 0x10, 0x65, 0x44, 0x8b, 0x14, 0x25, 0x48,
      0x00, 0x00, 0x00, 0x44, 0x89, 0x50, 0x04, 0x4c, 0x8b, 0x54, 0x24, 0x20,
      0x4c, 0x89, 0x50, 0x08, 0x41, 0x5b, 0x41, 0x5a, 0x58, 0x9d, 0xff, 0x25,
      0x00, 0x00, 0x00, 0x00, 0x84, 0x70, 0x60, 0x50, 0x40, 0x30, 0x20, 0x10,
  };
  auto* executable = static_cast<u8*>(be->alloc(sizeof(kTrace), 0));
  if (!executable) return fn;
  memcpy(executable, kTrace, sizeof(kTrace));
  const u64 patch_8 = reinterpret_cast<uintptr_t>(&g_recent_import_tick);
  memcpy(executable + 8, &patch_8, sizeof(patch_8));
  const u64 patch_37 = reinterpret_cast<uintptr_t>(g_recent_imports);
  memcpy(executable + 37, &patch_37, sizeof(patch_37));
  const u32 patch_50 = site_index;
  memcpy(executable + 50, &patch_50, sizeof(patch_50));
  const u64 patch_88 = reinterpret_cast<uintptr_t>(fn);
  memcpy(executable + 88, &patch_88, sizeof(patch_88));
  if (be->protect(executable, sizeof(kTrace), TRT_PROT_R | TRT_PROT_X) != 0) {
    be->release(executable, sizeof(kTrace));
    return fn;
  }
  return executable;
}
}

extern "C" void trt_log_recent_imports(u32 tid) {
  const u32 end = g_recent_import_tick.load(std::memory_order_relaxed);
  const u32 begin = end > 64 ? end - 64 : 0;
  for (u32 i = begin; i < end; ++i) {
    const RecentImport& event = g_recent_imports[i & 63u];
    if (event.tid != tid) continue;
    const u32 site = event.site;
    if (site >= sizeof(g_import_sites) / sizeof(g_import_sites[0])) continue;
    const ImportLocation& where = g_import_sites[site];
    char line[230];
    snprintf(line, sizeof(line), "recent guest import: %s!%s caller=%p",
             where.module, where.function, reinterpret_cast<void*>(event.caller));
    trt_core_log(line);
  }
}
#endif

#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
extern "C" void __asan_unpoison_memory_region(const volatile void*, size_t)
    __attribute__((weak));
static const auto asan_unpoison = __asan_unpoison_memory_region;
#  endif
#endif

namespace {

u16 rd16(const u8* p) { u16 v; memcpy(&v, p, 2); return v; }
u32 rd32(const u8* p) { u32 v; memcpy(&v, p, 4); return v; }
u64 rd64(const u8* p) { u64 v; memcpy(&v, p, 8); return v; }

void logf_be(const trt_backend* be, const char* msg) {
  if (be && be->log) be->log(msg);
}

// ---------------------------------------------------------------------------
// Per-thread guest TEB + dynamic .tls template instantiation:
// Guest code reads:
//   gs:[0x08] StackBase, gs:[0x10] StackLimit (MSVC stack checks),
//   gs:[0x30] Self pointer,
//   gs:[0x58] ThreadLocalStoragePointer -> thread's own array[index] -> block.
// Each mapped PE with a TLS directory registers a template (raw bytes +
// zero-fill size). Every OS thread materializes its own independent copy of
// every module's TLS block via epoch checking before entering guest code.
// ---------------------------------------------------------------------------
#define TRT_TEB_SIZE 0x4000u          // shared PEB/fallback storage
#define TRT_TEB_TLS_ARRAY_OFF 0x600u

#if defined(_WIN32)
// Leave the native loader's static TLS indices below the guest's range.
constexpr u32 k_guest_tls_first_index = 1024;
extern "C" unsigned long _tls_index;
#else
constexpr u32 k_guest_tls_first_index = 0;
#endif

struct tls_template {
  u32 index = 0;
  std::vector<u8> raw;
  size_t total_size = 0;
};

} // namespace

static std::mutex g_teb_mu;
static u8* g_guest_teb = nullptr;
static u32 g_guest_tls_next = k_guest_tls_first_index;
static std::atomic<u32> g_tls_epoch{1};
static std::vector<tls_template> g_tls_templates;

struct ThreadTlsState {
  alignas(16) u8 teb[0x1900]{};
  void** slots = nullptr;
  size_t cap = 0;
  std::vector<void**> retired_arrays;
  u32 epoch = 0;
  bool teb_inited = false;
  ~ThreadTlsState() {
    if (slots) {
      for (size_t i = k_guest_tls_first_index; i < cap; ++i) {
        free(slots[i]);
      }
      free(slots);
      slots = nullptr;
    }
    for (void** old : retired_arrays) free(old);
  }
};

static thread_local ThreadTlsState t_tls_state;

static void fill_teb_stack(u8* teb) {
  // NT_TIB offsets: gs:[0x08] = StackBase (high), gs:[0x10] = StackLimit (low, 4K-aligned).
  u64 stack_lim = ((u64)(uintptr_t)teb - 0x100000) & ~0xFFFULL;
  u64 stack_base = (u64)(uintptr_t)teb & ~0xFFFULL;
#if !defined(_WIN32)
  pthread_attr_t attr;
  void* sb = nullptr;
  size_t ss = 0;
  if (pthread_getattr_np(pthread_self(), &attr) == 0) {
    if (pthread_attr_getstack(&attr, &sb, &ss) == 0 && sb && ss) {
      stack_lim = ((u64)(uintptr_t)sb + 0xFFFULL) & ~0xFFFULL;
      stack_base = (((u64)(uintptr_t)sb + (u64)ss)) & ~0xFFFULL;
    }
    pthread_attr_destroy(&attr);
  }
#endif
  *(u64*)(teb + 0x08) = stack_base;  // StackBase  (high addr)
  *(u64*)(teb + 0x10) = stack_lim;   // StackLimit (low addr)
}

extern "C" u32 TRT_STDCALL trt_GetCurrentThreadId(void);

static u8* ensure_shared_teb() {
  std::lock_guard<std::mutex> lk(g_teb_mu);
  if (!g_guest_teb) {
    u8* teb = (u8*)calloc(1, TRT_TEB_SIZE);
    if (!teb) return nullptr;
    void** tls_array = (void**)(teb + TRT_TEB_TLS_ARRAY_OFF);
    fill_teb_stack(teb);
    // gs:[0x60] = PEB pointer (guest CRT reads it: gs:0x60 -> PEB+0x20
    // NumberOfProcessors etc.). Build a minimal PEB right after the TEB.
    u8* peb = teb + 0x2000;
    *(u64*)(teb + 0x60) = (u64)(uintptr_t)peb;
    static u8* s_pp;
    if (!s_pp) s_pp = (u8*)calloc(1, 0x400);
    *(u64*)(peb + 0x20) = (u64)(uintptr_t)s_pp;
    *(u64*)(peb + 0x28) = 0;
    *(u32*)(peb + 0xB8) = 8;                    // NumberOfProcessors
    *(u64*)(teb + 0x30) = (u64)(uintptr_t)teb;  // Self
    *(u64*)(teb + 0x40) = 0x1000;               // ClientId.UniqueProcess
    *(u64*)(teb + 0x48) = (u64)trt_GetCurrentThreadId();  // ClientId.UniqueThread
    *(void**)(teb + 0x58) = (void*)tls_array;
    g_guest_teb = teb;
  }
  return g_guest_teb;
}

static void sync_thread_tls_state() {
  ThreadTlsState& ts = t_tls_state;
  const u32 cur_epoch = g_tls_epoch.load(std::memory_order_acquire);
  if (ts.teb_inited && ts.epoch == cur_epoch && ts.slots != nullptr) return;

  u8* shared = ensure_shared_teb();
  if (!shared) return;

  if (!ts.teb_inited) {
    memset(ts.teb, 0, sizeof(ts.teb));
    fill_teb_stack(ts.teb);
    *(u64*)(ts.teb + 0x30) = (u64)(uintptr_t)ts.teb;  // Self
    *(u64*)(ts.teb + 0x40) = 0x1000;                  // ClientId.UniqueProcess
    *(u64*)(ts.teb + 0x48) = (u64)trt_GetCurrentThreadId();  // ClientId.UniqueThread
    *(u64*)(ts.teb + 0x60) = (u64)(uintptr_t)(shared + 0x2000);  // PEB
    ts.teb_inited = true;
  }

  std::lock_guard<std::mutex> lk(g_teb_mu);
  const size_t needed = (size_t)g_guest_tls_next;
  size_t target_cap = ts.cap ? ts.cap : 512;
  while (target_cap < needed) target_cap *= 2;
  if (!ts.slots || target_cap > ts.cap) {
    void** next = (void**)calloc(target_cap, sizeof(void*));
    if (!next) return;
    if (ts.slots) {
      memcpy(next, ts.slots, ts.cap * sizeof(void*));
      ts.retired_arrays.push_back(ts.slots);
    }
    ts.slots = next;
    ts.cap = target_cap;
  }

  for (const tls_template& tpl : g_tls_templates) {
    if (tpl.index < ts.cap && ts.slots[tpl.index] == nullptr) {
      const size_t alloc_sz = tpl.total_size ? tpl.total_size : 1;
      u8* block = (u8*)malloc(alloc_sz);
      if (!block) continue;
      if (!tpl.raw.empty()) memcpy(block, tpl.raw.data(), tpl.raw.size());
      if (alloc_sz > tpl.raw.size()) {
        memset(block + tpl.raw.size(), 0, alloc_sz - tpl.raw.size());
      }
      ts.slots[tpl.index] = block;
    }
  }

  *(void**)(ts.teb + 0x58) = (void*)ts.slots;
  ts.epoch = g_tls_epoch.load(std::memory_order_relaxed);
}

int trt_guest_tls_register(const u8* raw, size_t raw_size, size_t total_size,
                           u32* out_index) {
  if (!out_index) return -1;
  {
    std::lock_guard<std::mutex> lk(g_teb_mu);
    const u32 idx = g_guest_tls_next++;
    tls_template tpl;
    tpl.index = idx;
    tpl.total_size = total_size ? total_size : (raw_size ? raw_size : 1);
    if (raw && raw_size > 0) {
      tpl.raw.assign(raw, raw + raw_size);
    }
    g_tls_templates.push_back(std::move(tpl));
    g_tls_epoch.fetch_add(1, std::memory_order_release);
    *out_index = idx;
  }
  sync_thread_tls_state();
  return 0;
}

void trt_guest_tls_sync_thread(void) {
  sync_thread_tls_state();
}

void** trt_guest_tls_array(void) {
  sync_thread_tls_state();
  return t_tls_state.slots;
}

#if defined(_WIN32)
static void bind_native_module_tls(HMODULE module, void** native, void** guest) {
  if (!module) return;
  // The platform supplies mapped module handles. A module can still unload
  // between enumeration and this read, so guard the borrowed pointers.
  __try {
    const auto* base = reinterpret_cast<const u8*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_TLS) return;
    const auto& entry = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (!entry.VirtualAddress || entry.Size < sizeof(IMAGE_TLS_DIRECTORY64)) return;
    const auto* tls = reinterpret_cast<const IMAGE_TLS_DIRECTORY64*>(base + entry.VirtualAddress);
    if (!tls->AddressOfIndex) return;
    const DWORD index = *reinterpret_cast<const DWORD*>(tls->AddressOfIndex);
    if (index < k_guest_tls_first_index) guest[index] = native[index];
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
#endif

void trt_guest_tls_bind_host_slots(void** native, void** guest) {
#if defined(_WIN32)
  if (!native || !guest || native == guest) return;
  const u32 index = (u32)_tls_index;
  if (index < k_guest_tls_first_index) guest[index] = native[index];

  using enum_modules = BOOL (WINAPI*)(HANDLE, HMODULE*, DWORD, LPDWORD);
  static const auto modules = reinterpret_cast<enum_modules>(
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32EnumProcessModules"));
  HMODULE loaded[256];
  DWORD bytes = 0;
  if (modules && modules(GetCurrentProcess(), loaded, sizeof(loaded), &bytes)) {
    const size_t count = bytes < sizeof(loaded) ? bytes / sizeof(HMODULE)
                                               : sizeof(loaded) / sizeof(HMODULE);
    for (size_t i = 0; i < count; ++i) bind_native_module_tls(loaded[i], native, guest);
  } else {
    // The Xbox AppContainer can restrict process enumeration. Preserve the
    // loaded system components that the direct graphics path calls into.
    const wchar_t* graphics_modules[] = {
        L"d3d11.dll", L"d3d11on12.dll", L"d3d12.dll", L"D3D12Core.dll",
        L"dxgi.dll", L"d3dcompiler_47.dll", L"winrtbase.dll", L"combase.dll"};
    for (const auto* name : graphics_modules)
      bind_native_module_tls(GetModuleHandleW(name), native, guest);
  }
#else
  (void)native;
  (void)guest;
#endif
}

void* trt_guest_teb_acquire(u32* out_index) {
  u8* shared = ensure_shared_teb();
  if (!shared) return nullptr;
  if (out_index) {
    if (trt_guest_tls_register(nullptr, 0, 0, out_index) != 0) return nullptr;
    return shared;
  }
  sync_thread_tls_state();
  return t_tls_state.teb;
}

// fail: record stage detail, release mapping, return error code
int fail(trt_image* img, const trt_backend* be, int code, const char* detail) {
  if (detail && img) {
    strncpy(img->last_detail, detail, sizeof(img->last_detail) - 1);
    img->last_detail[sizeof(img->last_detail) - 1] = 0;
  }
  char line[320];
  snprintf(line, sizeof(line), "trt: FAIL %s: %s", trt_errstr(code),
           detail ? detail : "");
  logf_be(be, line);
  if (img && img->base && be) {
    be->release(img->base, (size_t)img->size_of_image);
    img->base = nullptr;
  }
  return code;
}

// section target protection per PE characteristics (pre-seal everything RW;
// seal flips exec to RX)
unsigned section_prot(bool exec) { return exec ? (TRT_PROT_R | TRT_PROT_X)
                                               : (TRT_PROT_R | TRT_PROT_W); }

extern "C" const char* trt_errstr(int code) {
  switch (code) {
    case TRT_OK: return "OK";
    case TRT_ERR_BAD_DOS: return "BAD_DOS (not MZ / e_lfanew out of file)";
    case TRT_ERR_BAD_PE: return "BAD_PE (no PE signature)";
    case TRT_ERR_NOT_PE64: return "NOT_PE64 (machine/magic mismatch, 32-bit needs the x86 engine, not this path)";
    case TRT_ERR_NO_SECTIONS: return "NO_SECTIONS";
    case TRT_ERR_SIZE_OVERFLOW: return "SIZE_OVERFLOW (SizeOfImage missing or over 2 GB cap)";
    case TRT_ERR_SECTION_BOUNDS: return "SECTION_BOUNDS (section points outside file/image)";
    case TRT_ERR_NO_BASE: return "NO_BASE (fixed base required but unavailable)";
    case TRT_ERR_ALLOC: return "ALLOC (backend allocation failed)";
    case TRT_ERR_RELOC_BADTYPE: return "RELOC_BADTYPE";
    case TRT_ERR_RELOC_BOUNDS: return "RELOC_BOUNDS";
    case TRT_ERR_IMPORT: return "IMPORT (unresolvable symbol)";
    case TRT_ERR_TLS_BOUNDS: return "TLS_BOUNDS";
    case TRT_ERR_SEAL: return "SEAL (final protect failed)";
    case TRT_ERR_DELAY_IMPORT: return "DELAY_IMPORT (delay-load directory not processed in v0)";
    case TRT_ERR_MANAGED: return "MANAGED (.NET assembly: CLR header present, no .NET Framework CLR on console)";
    default: return "UNKNOWN";
  }
}

extern "C" int trt_map_pe(const trt_backend* be, const trt_resolver* rs,
                           const u8* file, size_t file_size, trt_image** out) {
  if (!be || !rs || !file || !out || file_size < sizeof(trt_dos_header))
    return TRT_ERR_BAD_ARGS;
  *out = nullptr;

  trt_image img;
  memset(&img, 0, sizeof(img));

  // ---- 1. DOS header ----
  const trt_dos_header* dos = (const trt_dos_header*)file;
  if (rd16(file) != 0x5A4D) return fail(&img, be, TRT_ERR_BAD_DOS, "no MZ");
  u64 nt_off = dos->e_lfanew;
  if (nt_off < sizeof(trt_dos_header) || nt_off + sizeof(trt_nt_headers64) > file_size)
    return fail(&img, be, TRT_ERR_BAD_DOS, "e_lfanew out of file");

  // ---- 2. NT headers ----
  const trt_nt_headers64* nt = (const trt_nt_headers64*)(file + nt_off);
  if (rd32((const u8*)&nt->signature) != 0x00004550)
    return fail(&img, be, TRT_ERR_BAD_PE, "no PE\\0\\0");
  if (rd16((const u8*)&nt->file_header.machine) != TRT_PE_MACHINE_X64 ||
      rd16((const u8*)&nt->optional_header.magic) != TRT_PE_OPT64_MAGIC) {
    // Name a 32-bit image precisely: a PE32 with a COM descriptor (data
    // directory 14 in the PE32 optional header: NumberOfRvaAndSizes at
    // opt+92, directories at opt+96) is a managed .NET assembly — it needs
    // BOTH an x86 engine and a CLR, and neither exists on this path.
    // (Celeste.exe is exactly this: PE32, COM descriptor, sole import
    // mscoree.dll!_CorExeMain.) The e_lfanew bounds check above validated
    // 264 bytes of NT headers, which covers the PE32 directory table.
    if (rd16((const u8*)&nt->file_header.machine) == 0x014c &&
        rd16((const u8*)&nt->optional_header.magic) == 0x010b) {
      const u8* opt32 = (const u8*)&nt->optional_header;
      if (rd32(opt32 + 92) > TRT_PE_DIR_COM_DESCRIPTOR &&
          rd32(opt32 + 96 + TRT_PE_DIR_COM_DESCRIPTOR * 8) != 0)
        return fail(&img, be, TRT_ERR_NOT_PE64,
                    "32-bit managed .NET assembly (COM descriptor present); "
                    "needs x86 engine + CLR, neither available");
      return fail(&img, be, TRT_ERR_NOT_PE64,
                  "32-bit PE (0x014c/0x010b); only PE64 on this path");
    }
    return fail(&img, be, TRT_ERR_NOT_PE64, "only PE64 (0x8664/0x020b) on this path");
  }
  u32 nsec = rd16((const u8*)&nt->file_header.number_of_sections);
  if (nsec == 0 || nsec > 96)
    return fail(&img, be, TRT_ERR_NO_SECTIONS, "section count 0 or >96");
  u64 size_of_image = rd32((const u8*)&nt->optional_header.size_of_image);
  u64 section_alignment = rd32((const u8*)&nt->optional_header.section_alignment);
  if (size_of_image == 0 || size_of_image > TRT_PE_MAX_IMAGE_BYTES ||
      section_alignment < 4096)
    return fail(&img, be, TRT_ERR_SIZE_OVERFLOW, "SizeOfImage/alignment insane");

  const u64 preferred = rd64((const u8*)&nt->optional_header.image_base);
  const u64 entry_rva = rd32((const u8*)&nt->optional_header.address_of_entry_point);
  const u64 size_headers = rd32((const u8*)&nt->optional_header.size_of_headers);
  const trt_data_directory* dirs = nt->optional_header.directories;

  // ---- 2b. managed (.NET) assemblies ----
  // A PE with a COM descriptor (data directory 14) is a .NET assembly: its
  // entry point is a stub that jumps to mscoree.dll!_CorExeMain, and there
  // is no .NET Framework CLR on the console (no mscoree/clr.dll, no GAC,
  // no fusion, no XNA redist). Fail fast with a dedicated code instead of
  // dying later at import bind with an unexplained mscoree.dll error.
  // Deliberately NOT a STUB: binding _CorExeMain to a thunk that returns 0
  // would make the game "exit 0" silently, which is worse than failing.
  if (rd32((const u8*)&nt->optional_header.number_of_rva_and_sizes) >
          TRT_PE_DIR_COM_DESCRIPTOR &&
      dirs[TRT_PE_DIR_COM_DESCRIPTOR].virtual_address != 0)
    return fail(&img, be, TRT_ERR_MANAGED,
                "managed .NET assembly (COM descriptor present); no CLR on console");

  // section headers live right after optional header, in the file
  const u8* sec_base = (const u8*)&nt->optional_header +
                       rd16((const u8*)&nt->file_header.size_of_optional_header);
  if ((size_t)(sec_base - file) + (size_t)nsec * sizeof(trt_section_header) > file_size)
    return fail(&img, be, TRT_ERR_SECTION_BOUNDS, "section headers out of file");

  // ---- 3. allocate ----
  const u16 file_chars = rd16((const u8*)&nt->file_header.characteristics);
  const u16 dll_chars = rd16((const u8*)&nt->optional_header.dll_characteristics);
  const bool has_relocs = dirs[TRT_PE_DIR_RELOC].virtual_address != 0 &&
                          dirs[TRT_PE_DIR_RELOC].size != 0;
  // A /DYNAMICBASE image without RELOCS_STRIPPED whose .reloc directory is empty
  // contains zero absolute fixups (pure RIP-relative code) and can safely run
  // off preferred base (critical on Xbox UWP where VirtualAllocFromApp cannot
  // guarantee arbitrary ImageBase).
  const bool can_relocate =
      has_relocs ||
      ((dll_chars & 0x0040u /*IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE*/) &&
       !(file_chars & 0x0001u /*IMAGE_FILE_RELOCS_STRIPPED*/));
  void* base = be->alloc((size_t)size_of_image, preferred);
  if (!base && can_relocate) {        // retry anywhere; relocs (if any) will fix it up
    base = be->alloc((size_t)size_of_image, 0);
    if (base) logf_be(be, "trt: preferred base unavailable, relocated");
  }
  if (!base)
    return fail(&img, be, can_relocate ? TRT_ERR_ALLOC : TRT_ERR_NO_BASE,
                "VirtualAlloc/map failed");
  memset(base, 0, (size_t)size_of_image);

  img.base = (u8*)base;
  img.preferred_base = preferred;
  img.size_of_image = size_of_image;
  img.entry_rva = entry_rva;
  img.section_count = nsec;
  // exception directory (.pdata) — an array of RUNTIME_FUNCTION sorted by
  // BeginRva; the image copies it verbatim so unwind lookups can binary
  // search it after seal.
  {
    const u64 pd_va = dirs[TRT_PE_DIR_EXCEPTION].virtual_address;
    const u64 pd_sz = dirs[TRT_PE_DIR_EXCEPTION].size;
    if (pd_va && pd_sz >= 12 && pd_va + pd_sz <= size_of_image) {
      img.pdata = img.base + pd_va;
      img.pdata_count = (u32)(pd_sz / 12);
    }
  }

  // ---- 4. headers + sections ----
  if (size_headers > size_of_image || size_headers > file_size)
    return fail(&img, be, TRT_ERR_SECTION_BOUNDS, "SizeOfHeaders out of bounds");
  memcpy(img.base, file, (size_t)size_headers);

  u64 max_end = size_headers;
  for (u32 i = 0; i < nsec; ++i) {
    const trt_section_header* sh =
        (const trt_section_header*)(sec_base + (size_t)i * sizeof(trt_section_header));
    const u64 va = rd32((const u8*)&sh->virtual_address);
    const u64 vsize = rd32((const u8*)&sh->virtual_size);
    const u64 rsize = rd32((const u8*)&sh->size_of_raw_data);
    const u64 roff = rd32((const u8*)&sh->pointer_to_raw_data);
    const u64 mem_sz = vsize ? vsize : rsize;
    if (va + mem_sz > size_of_image)
      return fail(&img, be, TRT_ERR_SECTION_BOUNDS, "section VA+VSize > SizeOfImage");
    if (rsize) {
      if (roff + rsize > file_size)
        return fail(&img, be, TRT_ERR_SECTION_BOUNDS, "section raw data out of file");
      // SizeOfRawData is FileAlignment-padded on disk and can exceed VirtualSize
      // (and SizeOfImage) on the last section; PE loaders map at most mem_sz bytes.
      u64 copy_sz = (vsize && rsize > vsize) ? vsize : rsize;
      if (copy_sz > size_of_image - va) copy_sz = size_of_image - va;
      memcpy(img.base + va, file + roff, (size_t)copy_sz);
    }
    // BSS tail already zero from memset
    if (va + mem_sz > max_end) max_end = va + mem_sz;
    char line[96];
    snprintf(line, sizeof(line), "trt: sec[%u] %.8s va=%llx sz=%llx%s",
             i, (const char*)sh->name, (unsigned long long)va,
             (unsigned long long)vsize, (rd32((const u8*)&sh->characteristics) & TRT_SCN_MEM_EXEC) ? " exec" : "");
    logf_be(be, line);
  }
  if (max_end > size_of_image)
    return fail(&img, be, TRT_ERR_SECTION_BOUNDS, "sections exceed SizeOfImage");

  const u64 delta = (u64)(uintptr_t)img.base - preferred;

  // ---- 5. relocations (only when loaded off preferred base) ----
  if (delta != 0) {
    if (!can_relocate)
      return fail(&img, be, TRT_ERR_NO_BASE, "delta != 0 and no reloc directory");
    if (!has_relocs) {
      logf_be(be, "trt: dynamic-base image has empty .reloc (0 fixups); relocated");
    } else {
      const u64 dir_va = dirs[TRT_PE_DIR_RELOC].virtual_address;
      const u64 dir_sz = dirs[TRT_PE_DIR_RELOC].size;
    if (dir_va + dir_sz > size_of_image)
      return fail(&img, be, TRT_ERR_RELOC_BOUNDS, "reloc dir out of image");
    u64 off = 0;
    u32 n_fixed = 0;
    while (off + 8 <= dir_sz) {
      const u8* blk = img.base + dir_va + off;
      const u32 page_rva = rd32(blk);
      const u32 block_size = rd32(blk + 4);
      if (block_size < 8 || off + block_size > dir_sz)
        return fail(&img, be, TRT_ERR_RELOC_BOUNDS, "reloc block size corrupt");
      const u32 n_entries = (block_size - 8) / 2;
      for (u32 e = 0; e < n_entries; ++e) {
        const u16 entry = rd16(blk + 8 + (size_t)e * 2);
        const u32 type = entry >> 12;
        const u32 fo = entry & 0x0FFF;
        if (type == TRT_RELOC_ABSOLUTE) continue;
        const u64 tgt_rva = page_rva + fo;
        if (tgt_rva + 8 > size_of_image)
          return fail(&img, be, TRT_ERR_RELOC_BOUNDS, "reloc target out of image");
        u8* p = img.base + tgt_rva;
        if (type == TRT_RELOC_DIR64) {
          u64 v = rd64(p); v += delta; memcpy(p, &v, 8);
        } else if (type == TRT_RELOC_HIGHLOW) {
          if (tgt_rva + 4 > size_of_image)
            return fail(&img, be, TRT_ERR_RELOC_BOUNDS, "HIGHLOW out of image");
          u32 v = rd32(p); v += (u32)delta; memcpy(p, &v, 4);
        } else {
          char d[64]; snprintf(d, sizeof(d), "reloc type %u unsupported", type);
          return fail(&img, be, TRT_ERR_RELOC_BADTYPE, d);
        }
        ++n_fixed;
      }
      off += block_size;
    }
      char line[96];
      snprintf(line, sizeof(line), "trt: reloc delta=%llx fixed=%u",
               (unsigned long long)delta, n_fixed);
      logf_be(be, line);
    }
  } else {
    logf_be(be, "trt: loaded at preferred base, no reloc pass");
  }

  // Publish the allocated, section-mapped, relocated image to *out BEFORE
  // walking imports so circular DLL dependencies (e.g. libSceGnm.dll <->
  // libSceGpuAddress.dll) can resolve exports against this in-progress image
  // without re-entering trt_map_pe recursively.
  *out = (trt_image*)malloc(sizeof(trt_image));
  if (!*out) return fail(&img, be, TRT_ERR_ALLOC, "image struct alloc");
  memcpy(*out, &img, sizeof(img));
  auto fail_out = [&](int code, const char* detail) -> int {
    if (*out) {
      free(*out);
      *out = nullptr;
    }
    return fail(&img, be, code, detail);
  };

  // ---- 6. imports ----
  const u64 imp_va = dirs[TRT_PE_DIR_IMPORT].virtual_address;
  const u64 imp_sz = dirs[TRT_PE_DIR_IMPORT].size;
  u32 n_modules = 0, n_symbols = 0;
  if (imp_va && imp_sz) {
    if (imp_va + imp_sz > size_of_image)
      return fail_out(TRT_ERR_IMPORT, "import dir out of image");
    for (u64 d = 0; d + sizeof(trt_import_descriptor) <= imp_sz;
         d += sizeof(trt_import_descriptor)) {
      const trt_import_descriptor* desc =
          (const trt_import_descriptor*)(img.base + imp_va + d);
      if (!desc->first_thunk && !desc->name) break;  // terminator
      if (desc->name >= size_of_image || desc->first_thunk >= size_of_image)
        return fail_out(TRT_ERR_IMPORT, "import descriptor out of image");
      const char* mod_name = (const char*)(img.base + desc->name);
      const u64 int_rva = desc->original_first_thunk ? desc->original_first_thunk
                                                     : desc->first_thunk;
      const u64 iat_rva = desc->first_thunk;
      u32 bound = 0;
      for (u64 t = iat_rva;; t += 8) {
        const u64 lt = int_rva + (t - iat_rva);
        if (t + 8 > size_of_image || lt + 8 > size_of_image)
          return fail_out(TRT_ERR_IMPORT, "IAT runs off image");
        const u64 thunk = rd64(img.base + lt);
        if (thunk == 0) break;  // terminator
        const char* sym_name = nullptr;
        u16 ordinal = 0;
        if (thunk >> 63) {
          ordinal = (u16)(thunk & 0xFFFF);
        } else {
          if (thunk + 2 >= size_of_image)
            return fail_out(TRT_ERR_IMPORT, "hint/name out of image");
          sym_name = (const char*)(img.base + thunk + 2);
          if (strnlen(sym_name, 256) == 256)
            return fail_out(TRT_ERR_IMPORT, "symbol name not terminated");
        }
        void* fn = rs->resolve(rs->user, mod_name, sym_name, ordinal);
        if (!fn) {
          char det[224];
          snprintf(det, sizeof(det), "%s!%s", mod_name,
                   sym_name ? sym_name : "#ordinal");
          return fail_out(TRT_ERR_IMPORT, det);
        }
#if defined(_WIN32)
        fn = trace_guest_import(be, mod_name, sym_name, ordinal, fn);
#endif
        memcpy(img.base + t, &fn, 8);  // bind IAT
        if (n_symbols == 0 || strncmp(mod_name, "steam_api", 9) == 0) {
          char line[256];
          snprintf(line, sizeof(line), "trt: bind %s!%s -> %p", mod_name,
                   sym_name ? sym_name : "#ordinal", fn);
          logf_be(be, line);
        }
        ++bound; ++n_symbols;
      }
      char line[160];
      snprintf(line, sizeof(line), "trt: import %s bound=%u", mod_name, bound);
      logf_be(be, line);
      ++n_modules;
    }
  } else {
    logf_be(be, "trt: no import directory");
  }
  (void)n_modules; (void)n_symbols;

  // ---- 7. TLS callbacks (collected; caller runs them after seal) ----
  const u64 tls_va = dirs[TRT_PE_DIR_TLS].virtual_address;
  if (tls_va) {
    if (tls_va + sizeof(trt_tls_directory64) > size_of_image)
      return fail_out(TRT_ERR_TLS_BOUNDS, "tls dir out of image");
    const trt_tls_directory64* tls = (const trt_tls_directory64*)(img.base + tls_va);
    // A TLS directory with AddressOfCallbacks == 0 is legal (TLS data, no
    // callbacks): zero callbacks, NOT a load failure. Without this guard
    // the VA math below underflows to a huge rva and dies TLS_BOUNDS.
    if (tls->address_of_callbacks == 0) {
      img.tls_callback_count = 0;
      logf_be(be, "trt: tls dir: AddressOfCallbacks==0, no callbacks");
    } else {
      // callbacks array is addressed by VA; after relocation it points at the
      // RUNTIME image (new base + rva), so translate through the actual base —
      // NOT the preferred ImageBase. Subtracting `preferred` only worked when
      // the image loaded at its preferred base (delta==0); the first relocated
      // TLS-bearing DLL (baselib.dll inside GameAssembly's run) produced a
      // huge bogus rva and died TLS_BOUNDS. STAGE 6 run-depth found it.
      const u64 arr_rva =
          (u64)tls->address_of_callbacks - (u64)(uintptr_t)img.base;
      if (arr_rva + 8 > size_of_image)
        return fail_out(TRT_ERR_TLS_BOUNDS, "tls callback array out of image");
      u64 c = arr_rva;
      for (; c + 8 <= size_of_image &&
             img.tls_callback_count < TRT_MAX_TLS_CALLBACKS; c += 8) {
        const u64 cb = rd64(img.base + c);
        if (cb == 0) break;
        const u64 cb_rva = cb - (u64)(uintptr_t)img.base;
        if (cb_rva >= size_of_image)
          return fail_out(TRT_ERR_TLS_BOUNDS, "tls callback out of image");
        img.tls_callback_rvas[img.tls_callback_count++] = cb_rva;
      }
      // the cap is a real limit, not a silent drop: say so when entries remain
      if (img.tls_callback_count >= TRT_MAX_TLS_CALLBACKS &&
          c + 8 <= size_of_image && rd64(img.base + c) != 0) {
        char capline[128];
        snprintf(capline, sizeof(capline),
                 "trt: tls callbacks truncated at cap=%d (more entries present)",
                 TRT_MAX_TLS_CALLBACKS);
        logf_be(be, capline);
      }
    }
    char line[96];
    snprintf(line, sizeof(line), "trt: tls callbacks=%d", img.tls_callback_count);
    logf_be(be, line);

    // ---- 7b. guest TLS array (STAGE 7): Windows stores a per-thread pointer
    // to an array of module TLS blocks at gs:[0x58] (the TEB's
    // ThreadLocalStoragePointer), and the module's TLS *index* at the
    // address_of_index slot. Guest code compiled with __declspec(thread)
    // reads its block as gs:[0x58] -> array -> array[index]. Without gs
    // pointing at a TEB, the first such access faults (measured live under
    // gdb: UnityPlayer.dll DllMain, mov %gs:0x58,%rax). We build the real
    // structure: index slot = the next free TLS slot, block = the raw TLS
    // data (copy + zero-fill) in a never-freed allocation, TEB slot = the
    // array. trt_tls_set_gs_base() installs the TEB as the guest gs base
    // before the entry runs (host gs must be preserved, so the guest gs
    // base is set per-call in trt_call_guest and restored after).
    {
      struct tls_dir_full {
        u64 start, end, index_va, callbacks_va;
        u32 zero_fill, chars;
      };
      const tls_dir_full* tf = (const tls_dir_full*)(img.base + tls_va);
      const u64 raw_start_rva = (u64)tf->start - (u64)(uintptr_t)img.base;
      const u64 raw_end_rva = (u64)tf->end - (u64)(uintptr_t)img.base;
      const u64 raw_size =
          raw_end_rva > raw_start_rva ? raw_end_rva - raw_start_rva : 0;
      const u64 block_size = raw_size + tf->zero_fill;
      const u64 index_rva = (u64)tf->index_va - (u64)(uintptr_t)img.base;
      char probe[160];
      snprintf(probe, sizeof(probe),
               "trt: tls dir probe: start_rva=%llx end_rva=%llx index_rva=%llx zero_fill=%u block=%llu",
               (unsigned long long)raw_start_rva,
               (unsigned long long)raw_end_rva,
               (unsigned long long)index_rva, tf->zero_fill,
               (unsigned long long)block_size);
      logf_be(be, probe);
      if (index_rva + 4 <= size_of_image && block_size > 0) {
        u32 tls_index = 0;
        const u8* raw_ptr = raw_size ? (img.base + raw_start_rva) : nullptr;
        if (trt_guest_tls_register(raw_ptr, (size_t)raw_size,
                                   (size_t)block_size, &tls_index) == 0) {
          img.guest_teb = ensure_shared_teb();
          *(u32*)(img.base + index_rva) = tls_index;
          char tline[160];
          snprintf(tline, sizeof(tline),
                   "trt: tls block size=%llu index=%u (per-thread TEB templates)",
                   (unsigned long long)block_size, tls_index);
          logf_be(be, tline);
        }
      }
    }
  }

  // ---- 8. delay imports (delay-load directory) ----
  // Games delay-load optional DLLs (d3dcompiler_47, xaudio2_9, ...) and
  // expect LoadLibrary to fail gracefully when they are absent. We bind
  // them EAGERLY: resolvable symbols get the real address, unresolvable
  // ones get a fail-visible thunk so the game still boots and the log
  // names the gap instead of the load dying.
  if (dirs[TRT_PE_DIR_DELAY_IMPORT].virtual_address) {
    const u64 dd_va = dirs[TRT_PE_DIR_DELAY_IMPORT].virtual_address;
    const u64 dd_sz = dirs[TRT_PE_DIR_DELAY_IMPORT].size;
    u32 n_delay_mods = 0, n_delay_bound = 0, n_delay_stub = 0;
    for (u64 d = 0; d + sizeof(trt_delayload_descriptor) <= (dd_sz ? dd_sz : 4096);
         d += sizeof(trt_delayload_descriptor)) {
      if (dd_va + d + sizeof(trt_delayload_descriptor) > size_of_image) break;
      const trt_delayload_descriptor* dd =
          (const trt_delayload_descriptor*)(img.base + dd_va + d);
      if (!dd->name_rva && !dd->iat_rva) break;  // terminator
      const int rva_based = (dd->attributes & 1u) != 0;
      // legacy descriptors store VAs: convert to RVAs
      const u64 name_rva = rva_based ? dd->name_rva
                                     : (u64)dd->name_rva - preferred;
      const u64 iat_rva = rva_based ? dd->iat_rva : (u64)dd->iat_rva - preferred;
      const u64 int_rva = rva_based ? dd->int_rva : (u64)dd->int_rva - preferred;
      const u64 mod_rva = rva_based ? dd->module_handle_rva
                                    : (u64)dd->module_handle_rva - preferred;
      if (name_rva >= size_of_image || iat_rva >= size_of_image ||
          int_rva >= size_of_image)
        return fail_out(TRT_ERR_DELAY_IMPORT, "delay descriptor out of image");
      const char* mod_name = (const char*)(img.base + name_rva);
      if (strnlen(mod_name, 256) == 256)
        return fail_out(TRT_ERR_DELAY_IMPORT, "delay dll name unterminated");
      for (u64 t = 0;; t += 8) {
        if (int_rva + t + 8 > size_of_image || iat_rva + t + 8 > size_of_image)
          break;
        const u64 thunk = rd64(img.base + int_rva + t);
        if (thunk == 0) break;
        const char* sym_name = nullptr;
        u16 ordinal = 0;
        if (thunk >> 63) {
          ordinal = (u16)(thunk & 0xFFFF);
        } else {
          if (thunk + 2 >= size_of_image) break;
          sym_name = (const char*)(img.base + thunk + 2);
          if (strnlen(sym_name, 256) == 256) break;
        }
        void* fn = rs->resolve(rs->user, mod_name, sym_name, ordinal);
        if (fn) {
          memcpy(img.base + iat_rva + t, &fn, 8);
          ++n_delay_bound;
        } else {
          char sym[128];
          if (sym_name) snprintf(sym, sizeof(sym), "%s", sym_name);
          else snprintf(sym, sizeof(sym), "#%u", (unsigned)ordinal);
          void* stub = trt_thunk_make(be, "DELAY", mod_name, sym);
          if (!stub) stub = (void*)&trt_stub_thunk;
          memcpy(img.base + iat_rva + t, &stub, 8);
          ++n_delay_stub;
        }
      }
      // the guest may ask for the module handle: give it a non-null sentinel
      if (mod_rva && mod_rva + 8 <= size_of_image) {
        u64 sentinel = 0x1000;
        memcpy(img.base + mod_rva, &sentinel, 8);
      }
      char line[160];
      snprintf(line, sizeof(line), "trt: DELAY import %s bound=%u stub=%u",
               mod_name, n_delay_bound, n_delay_stub);
      logf_be(be, line);
      ++n_delay_mods;
    }
    char line[128];
    snprintf(line, sizeof(line), "trt: delay imports modules=%u bound=%u stub=%u",
             n_delay_mods, n_delay_bound, n_delay_stub);
    logf_be(be, line);
  }

  memcpy(*out, &img, sizeof(img));
  char line[160];
  snprintf(line, sizeof(line),
           "trt: MAPPED base=%llx preferred=%llx size=%llx entry=%llx",
           (unsigned long long)(uintptr_t)img.base,
           (unsigned long long)preferred,
           (unsigned long long)size_of_image, (unsigned long long)entry_rva);
  logf_be(be, line);
#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
  // ASan cannot see mmap-backed guest memory: every legitimate guest write
  // into its own image reads as a wild "unknown-crash" (measured: the D3D
  // surface memsets at the image base). Unpoison the whole image so ASan
  // supervises guest writes as valid memory — real overflows past the image
  // are still caught.
  if (asan_unpoison) asan_unpoison(img.base, size_of_image);
#  endif
#endif
  return TRT_OK;
}

extern "C" int trt_seal_image(const trt_backend* be, trt_image* img) {
  if (!be || !img || !img->base) return TRT_ERR_BAD_ARGS;
  // walk section table in the mapped copy of the headers (NT headers sit at
  // e_lfanew, not at offset 0 — the DOS header occupies the page start)
  const trt_dos_header* dos = (const trt_dos_header*)img->base;
  const trt_nt_headers64* nt =
      (const trt_nt_headers64*)(img->base + dos->e_lfanew);
  const u8* sec_base = (const u8*)&nt->optional_header +
                       rd16((const u8*)&nt->file_header.size_of_optional_header);
  for (u32 i = 0; i < img->section_count; ++i) {
    const trt_section_header* sh =
        (const trt_section_header*)(sec_base + (size_t)i * sizeof(trt_section_header));
    const u32 ch = rd32((const u8*)&sh->characteristics);
    const u64 va = rd32((const u8*)&sh->virtual_address);
    const u64 vsize = rd32((const u8*)&sh->virtual_size);
    const bool exec = (ch & TRT_SCN_MEM_EXEC) != 0;
    const unsigned prot = section_prot(exec);
    if (be->protect(img->base + va, (size_t)vsize ? (size_t)vsize : 4096, prot)) {
      char d[96];
      snprintf(d, sizeof(d), "protect failed on section %u (prot=%u)", i, prot);
      return fail(img, be, TRT_ERR_SEAL, d);
    }
  }
  logf_be(be, "trt: SEALED (W^X: code pages now RX, no RWX ever)");

  // ---- unwind registration: the OS unwinder cannot see VirtualAlloc'd code
  // until its .pdata is registered. Without this, the FIRST guest C++
  // exception kills the whole process. This is boot-critical, not optional.
  const trt_data_directory* ex =
      &nt->optional_header.directories[TRT_PE_DIR_EXCEPTION];
  if (ex->virtual_address && ex->size && be->register_unwind) {
    const u32 count = ex->size / 12;  // RUNTIME_FUNCTION = 12 bytes (3 x u32)
    const int rc = be->register_unwind(img->base, img->base + ex->virtual_address,
                                       count);
    char line[128];
    snprintf(line, sizeof(line),
             "trt: unwind .pdata registered funcs=%u rc=%d (RtlAddFunctionTable)",
             count, rc);
    logf_be(be, line);
  } else if (ex->virtual_address && ex->size) {
    logf_be(be, "trt: NOTE .pdata present but backend cannot register unwind "
               "(exceptions in guest code will be fatal)");
  }
  return TRT_OK;
}

extern "C" void trt_unmap(const trt_backend* be, trt_image* img) {
  if (!be || !img) return;
  if (img->base) be->release(img->base, (size_t)img->size_of_image);
  free(img);
}

// ---------------------------------------------------------------------------
// Boot audit: parse import + delay-import tables IN FILE SPACE and push
// every module!symbol through the REAL resolver. No mapping, no execution,
// the file bytes are never modified. This is the offline gate for real
// games: it converts "should boot" into a measured, named list.
// ---------------------------------------------------------------------------
namespace {
// file-space RVA -> file offset (headers only; no allocation)
struct file_view {
  const u8* file;
  size_t size;
  u64 sec_off;   // file offset of section headers
  u32 nsec;
};

bool fv_rva2off(const file_view& fv, u64 rva, u64* out_off) {
  for (u32 s = 0; s < fv.nsec; ++s) {
    const u8* sh = fv.file + fv.sec_off + (size_t)s * 40;
    const u32 vsize = rd32(sh + 8);
    const u32 vaddr = rd32(sh + 12);
    const u32 rawsz = rd32(sh + 16);
    const u32 raw = rd32(sh + 20);
    if (vaddr <= rva && rva < vaddr + (vsize ? vsize : rawsz)) {
      if (rva - vaddr >= rawsz && rawsz) return false;  // in virtual padding
      *out_off = raw + (rva - vaddr);
      return *out_off < fv.size;
    }
  }
  return false;
}
}  // namespace

using ::trt_resolve_module;

extern "C" int trt_audit_imports(const u8* file, size_t file_size,
                                  void* missing_out) {
  if (!file || file_size < sizeof(trt_dos_header) || !missing_out)
    return TRT_ERR_BAD_ARGS;
  std::vector<std::string>* missing = (std::vector<std::string>*)missing_out;
  missing->clear();

  const trt_dos_header* dos = (const trt_dos_header*)file;
  if (rd16(file) != 0x5A4D) return TRT_ERR_BAD_DOS;
  const u64 nt_off = dos->e_lfanew;
  if (nt_off + sizeof(trt_nt_headers64) > file_size) return TRT_ERR_BAD_DOS;
  const trt_nt_headers64* nt = (const trt_nt_headers64*)(file + nt_off);
  if (rd32((const u8*)&nt->signature) != 0x00004550) return TRT_ERR_BAD_PE;
  if (rd16((const u8*)&nt->file_header.machine) != TRT_PE_MACHINE_X64 ||
      rd16((const u8*)&nt->optional_header.magic) != TRT_PE_OPT64_MAGIC)
    return TRT_ERR_NOT_PE64;
  const u32 nsec = rd16((const u8*)&nt->file_header.number_of_sections);
  if (nsec == 0 || nsec > 96) return TRT_ERR_NO_SECTIONS;
  const u64 size_of_image =
      rd32((const u8*)&nt->optional_header.size_of_image);
  const u64 preferred =
      rd64((const u8*)&nt->optional_header.image_base);
  const trt_data_directory* dirs = nt->optional_header.directories;
  // Managed (.NET) assemblies never reach import bind: the CLR header is
  // the verdict (same check as trt_map_pe step 2b).
  if (rd32((const u8*)&nt->optional_header.number_of_rva_and_sizes) >
          TRT_PE_DIR_COM_DESCRIPTOR &&
      dirs[TRT_PE_DIR_COM_DESCRIPTOR].virtual_address != 0)
    return TRT_ERR_MANAGED;
  const u64 sec_off = (u64)((const u8*)&nt->optional_header +
      rd16((const u8*)&nt->file_header.size_of_optional_header) - file);
  file_view fv{file, file_size, sec_off, nsec};

  // the real resolver — identical to trt_map_pe's import bind
  trt_resolver rs;
  trt_resolver_default(&rs);

  u64 total = 0, resolved = 0;
  char line[256];

  // ---- regular imports (these are hard: the loader fails the map) ----
  const u64 imp_va = dirs[TRT_PE_DIR_IMPORT].virtual_address;
  const u64 imp_sz = dirs[TRT_PE_DIR_IMPORT].size;
  if (imp_va && imp_sz) {
    for (u64 d = 0; d + sizeof(trt_import_descriptor) <= imp_sz;
         d += sizeof(trt_import_descriptor)) {
      u64 doff;
      if (!fv_rva2off(fv, imp_va + d, &doff)) break;
      const trt_import_descriptor desc = [&] {
        trt_import_descriptor v;
        memcpy(&v, file + doff, sizeof(v));
        return v;
      }();
      if (!desc.first_thunk && !desc.name) break;
      u64 name_off;
      if (!fv_rva2off(fv, desc.name, &name_off)) {
        missing->push_back("<bad>DLL-name-unreadable");
        continue;
      }
      if (name_off >= file_size) break;
      const char* mod_name = (const char*)(file + name_off);
      if (strnlen(mod_name, 256) == 256) break;
      const u32 ilt_rva = desc.original_first_thunk ? desc.original_first_thunk
                                                    : desc.first_thunk;
      for (u64 t = 0;; t += 8) {
        u64 toff;
        if (!fv_rva2off(fv, ilt_rva + t, &toff)) break;
        if (toff + 8 > file_size) break;
        const u64 thunk = rd64(file + toff);
        if (thunk == 0) break;
        const char* sym_name = nullptr;
        u16 ordinal = 0;
        if (thunk >> 63) {
          ordinal = (u16)(thunk & 0xFFFF);
        } else {
          u64 hoff;
          if (!fv_rva2off(fv, thunk & 0x7fffffffULL, &hoff) ||
              hoff + 2 >= file_size)
            break;
          sym_name = (const char*)(file + hoff + 2);
          if (strnlen(sym_name, 256) == 256) break;
        }
        ++total;
        // resolve WITHOUT a system loader (the console g_system_load is
        // set at runtime; offline we audit bridges + module defaults)
        void* fn = trt_resolve_module(nullptr, mod_name, sym_name, ordinal);
        if (fn) {
          ++resolved;
        } else {
          snprintf(line, sizeof(line), "%s!%s", mod_name,
                   sym_name ? sym_name : "#ordinal");
          missing->push_back(line);
        }
      }
    }
  }

  // ---- delay imports (soft: the loader stubs these fail-visibly) ----
  const u64 dd_va = dirs[TRT_PE_DIR_DELAY_IMPORT].virtual_address;
  const u64 dd_sz = dirs[TRT_PE_DIR_DELAY_IMPORT].size;
  if (dd_va && dd_sz) {
    u64 delay_missing = 0;
    for (u64 d = 0; d + sizeof(trt_delayload_descriptor) <= (dd_sz ? dd_sz : 4096);
         d += sizeof(trt_delayload_descriptor)) {
      u64 doff;
      if (!fv_rva2off(fv, dd_va + d, &doff)) break;
      if (doff + sizeof(trt_delayload_descriptor) > file_size) break;
      trt_delayload_descriptor dd;
      memcpy(&dd, file + doff, sizeof(dd));
      if (!dd.name_rva && !dd.iat_rva) break;
      const int rva_based = (dd.attributes & 1u) != 0;
      const u64 name_rva = rva_based ? dd.name_rva
                                     : (u64)dd.name_rva - preferred;
      const u64 int_rva = rva_based ? dd.int_rva
                                    : (u64)dd.int_rva - preferred;
      u64 name_off;
      if (name_rva >= size_of_image ||
          !fv_rva2off(fv, name_rva, &name_off))
        break;
      const char* mod_name = (const char*)(file + name_off);
      if (strnlen(mod_name, 256) == 256) break;
      for (u64 t = 0;; t += 8) {
        u64 toff;
        if (!fv_rva2off(fv, int_rva + t, &toff)) break;
        if (toff + 8 > file_size) break;
        const u64 thunk = rd64(file + toff);
        if (thunk == 0) break;
        const char* sym_name = nullptr;
        u16 ordinal = 0;
        if (thunk >> 63) {
          ordinal = (u16)(thunk & 0xFFFF);
        } else {
          u64 hoff;
          if (!fv_rva2off(fv, thunk & 0x7fffffffULL, &hoff) ||
              hoff + 2 >= file_size)
            break;
          sym_name = (const char*)(file + hoff + 2);
          if (strnlen(sym_name, 256) == 256) break;
        }
        ++total;
        // delay imports get fail-visible stubs at load: never a boot blocker
        if (trt_resolve_module(nullptr, mod_name, sym_name, ordinal))
          ++resolved;
        else
          ++delay_missing;
      }
    }
    if (delay_missing) {
      snprintf(line, sizeof(line),
               "(delay imports unresolved: %llu — stubbed fail-visible at "
               "load, not boot-blocking)",
               (unsigned long long)delay_missing);
      missing->push_back(line);
    }
  }

  snprintf(line, sizeof(line), "audit: %llu/%llu imports resolve",
           (unsigned long long)resolved, (unsigned long long)total);
  trt_core_log(line);
  return TRT_OK;
}
