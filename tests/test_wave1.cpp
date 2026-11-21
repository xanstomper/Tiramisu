// tiramisu/tests/test_wave1.cpp
// Verification suite for Wave 1 upgrades:
//   Wave 1A: Streaming windowed file mappings, view unmap reclamation,
//            named OpenFileMappingW, CloseHandle(hFile) before MapViewOfFile,
//            writable FlushViewOfFile writeback, >64 mappings/open files.
#include "../src/trt/trt.h"
#include "../src/trt/pe.h"
#include "../src/trt/thunk.h"
#include "../src/trt/bridge_mem.h"
#include "../src/trt/images.h"
#include "../src/trt/input.h"
#include "../src/trt/apiset.h"

#include <atomic>
#include <chrono>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern "C" {
void* TRT_STDCALL trt_CreateFileW(const u16* name, u32 access, u32 share,
                                  void* sa, u32 disp, u32 flags, void* tmpl);
int TRT_STDCALL trt_ReadFile(void* h, void* buf, u32 bytes, u32* read, void* ov);
int TRT_STDCALL trt_WriteFile(void* h, const void* buf, u32 bytes, u32* written,
                              void* ov);
int TRT_STDCALL trt_CloseHandle(void* h);
u32 TRT_STDCALL trt_SetFilePointer(void* h, i64 dist, i64* dist_hi, u32 method);
u32 TRT_STDCALL trt_GetLastError(void);
void TRT_STDCALL trt_SetLastError(u32 e);
u64 TRT_STDCALL trt_CreateFileMappingW(u64 hfile, void* attrs, u32 protect,
                                       u32 max_high, u32 max_low,
                                       const u16* name);
u64 TRT_STDCALL trt_OpenFileMappingW(u32 access, int inherit, const u16* name);
void* TRT_STDCALL trt_MapViewOfFile(u64 h, u32 access, u32 off_high,
                                    u32 off_low, u64 bytes);
int TRT_STDCALL trt_FlushViewOfFile(void* view, u64 bytes);
int TRT_STDCALL trt_UnmapViewOfFile(void* view);

u64 TRT_STDCALL trt_CreateThread(void* attrs, void* stack_size, void* routine,
                                 void* param, u32 flags, u32* tid);
u32 TRT_STDCALL trt_WaitForSingleObject(u64 handle, u32 ms);
u32 TRT_STDCALL trt_WaitForMultipleObjects(u32 count, const u64* handles,
                                           bool wait_all, u32 ms);
u32 TRT_STDCALL trt_SuspendThread(u64 thread);
u32 TRT_STDCALL trt_ResumeThread(u64 thread);
int TRT_STDCALL trt_GetThreadContext(u64 thread, void* ctx);
int TRT_STDCALL trt_SetThreadContext(u64 thread, const void* ctx);
u64 TRT_STDCALL trt_CreateEventW(void* attrs, bool manual, bool initial,
                                 const u16* name);
int TRT_STDCALL trt_SetEvent(u64 handle);
u64 TRT_STDCALL trt_CreateIoCompletionPort(void* file_handle,
                                           u64 existing_port,
                                           u64 completion_key,
                                           u32 concurrent_threads);
int TRT_STDCALL trt_GetQueuedCompletionStatus(u64 port, u32* bytes, u64* key,
                                              void** overlapped, u32 ms);
int TRT_STDCALL trt_GetOverlappedResult(void* h, void* ovl, u32* bytes,
                                        int wait);
void* TRT_STDCALL trt_AddVectoredExceptionHandler(u32 first, void* handler);
u32 TRT_STDCALL trt_RemoveVectoredExceptionHandler(void* handle);
void TRT_STDCALL trt_RaiseException(u32 code, u32 flags, u32 count, u64* args);
void* TRT_STDCALL trt_CreateWindowExW(u32 exstyle, const wchar_t* cls,
                                      const wchar_t* title, u32 style,
                                      int x, int y, int w, int h,
                                      void* parent, void* menu, void* inst,
                                      void* param);
int TRT_STDCALL trt_DestroyWindow(void* hwnd);
u64 TRT_STDCALL trt_SetTimer(void* hwnd, u64 id, u32 ms, void* callback);
int TRT_STDCALL trt_KillTimer(void* hwnd, u64 id);
int TRT_STDCALL trt_PeekMessageW(void* out, void* hwnd, u32 min, u32 max,
                                 u32 remove);
i64 TRT_STDCALL trt_DispatchMessageW(const void* m);
u32 TRT_STDCALL trt_GetCurrentPackageFullName(u32* len, u16* full_name);
int TRT_GUEST_ABI trt_CreateProcessW(const u16* app, u16* cmdline,
                                     void* proc_attr, void* thread_attr,
                                     int inherit, u32 flags, void* env,
                                     const u16* dir, const void* startup,
                                     void* info_out);
void* TRT_STDCALL trt_VirtualAlloc(void* addr, u64 bytes, u32 type, u32 prot);
int TRT_STDCALL trt_VirtualProtect(void* addr, u64 bytes, u32 prot, u32* oldp);
int TRT_STDCALL trt_VirtualFree(void* addr, u64 bytes, u32 type);
int TRT_STDCALL trt_WaitOnAddress(volatile void* addr, void* cmp,
                                  u64 addr_size, u32 ms);
void TRT_STDCALL trt_WakeByAddressSingle(volatile void* addr);
void TRT_STDCALL trt_WakeByAddressAll(volatile void* addr);
int trt_dispatch_hw_fault(int sig, void* siginfo, void* ucontext);
int TRT_STDCALL trt_SetDllDirectoryW(const u16* dir);
u32 TRT_STDCALL trt_GetDllDirectoryW(u32 buf_len, u16* buf);
void* TRT_STDCALL trt_AddDllDirectory(const u16* new_dir);
int TRT_STDCALL trt_RemoveDllDirectory(void* cookie);
int TRT_STDCALL trt_SetDefaultDllDirectories(u32 flags);
i32 TRT_STDCALL trt_AppPolicyGetProcessTerminationMethod(void* token, u32* method);
void* TRT_STDCALL trt_LoadLibraryW(const u16* path);
int TRT_STDCALL trt_FreeLibrary(void* handle);
void* TRT_STDCALL trt_GetProcAddress(void* handle, const char* name);
}

namespace {

void* mmap_alloc(size_t bytes, u64 preferred) {
  (void)preferred;
  void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return (p == MAP_FAILED) ? nullptr : p;
}

int mmap_protect(void* base, size_t bytes, unsigned prot) {
  int p = 0;
  if (prot & TRT_PROT_R) p |= PROT_READ;
  if (prot & TRT_PROT_W) p |= PROT_WRITE;
  if (prot & TRT_PROT_X) p |= PROT_EXEC;
  const uintptr_t addr = (uintptr_t)base;
  const uintptr_t page = addr & ~(uintptr_t)4095ULL;
  const size_t span = (size_t)((addr + bytes) - page);
  const size_t rounded = (span + 4095ULL) & ~(size_t)4095ULL;
  return mprotect((void*)page, rounded, p) == 0 ? 0 : -1;
}

void mmap_free(void* base, size_t bytes) {
  if (base) munmap(base, bytes);
}

void quiet_log(const char* s) { (void)s; }

int g_failures = 0;
#define CHECK(cond, msg)                         \
  do {                                           \
    if (cond) {                                  \
      printf("  PASS %s\n", msg);                \
    } else {                                     \
      printf("  FAIL %s\n", msg);                \
      ++g_failures;                              \
    }                                            \
  } while (0)

void make_u16(const char* s, u16* out, size_t cap) {
  size_t i = 0;
  for (; s[i] && i + 1 < cap; ++i) out[i] = (u16)(unsigned char)s[i];
  out[i] = 0;
}

// Guest TLS isolation test state
struct TlsWorkerArgs {
  u32 tls_index;
  u32 write_val;
  u32 read_initial;
  u32 read_after;
  u64 ready_ev;
  u64 proceed_ev;
};

u64 TRT_GUEST_ABI guest_tls_worker(u64 param) {
  TlsWorkerArgs* a = (TlsWorkerArgs*)(uintptr_t)param;
  void** tls_arr = trt_guest_tls_array();
  u32* slot = (u32*)tls_arr[a->tls_index];
  a->read_initial = slot ? *slot : 0u;
  if (slot) *slot = a->write_val;
  trt_SetEvent(a->ready_ev);
  trt_WaitForSingleObject(a->proceed_ev, 5000);
  a->read_after = slot ? *slot : 0u;
  return 0;
}

std::atomic<bool> g_susp_spin{true};
std::atomic<bool> g_susp_entered{false};
u64 TRT_GUEST_ABI guest_spin_worker(u64 param) {
  (void)param;
  g_susp_entered.store(true, std::memory_order_release);
  while (g_susp_spin.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  return 0x42;
}

u64 TRT_GUEST_ABI guest_suspended_override(u64 param) {
  return param + 100u;
}

std::vector<int> g_veh_trace;
i32 TRT_GUEST_ABI veh_head(void* ep) {
  (void)ep;
  g_veh_trace.push_back(1);
  return 0;  // EXCEPTION_CONTINUE_SEARCH
}
i32 TRT_GUEST_ABI veh_mid(void* ep) {
  (void)ep;
  g_veh_trace.push_back(2);
  return 0;
}
i32 TRT_GUEST_ABI veh_tail_catch(void* ep) {
  (void)ep;
  g_veh_trace.push_back(3);
  return -1;  // EXCEPTION_CONTINUE_EXECUTION
}

std::atomic<int> g_timer_ticks{0};
void TRT_GUEST_ABI guest_timer_cb(void* hwnd, u32 msg, u64 id, u32 time) {
  (void)hwnd; (void)msg; (void)time;
  if (id == 42u) g_timer_ticks.fetch_add(1, std::memory_order_relaxed);
}

u32 g_hw_safe_word = 0x12345678u;
u64 g_hw_expected_pc = 0;
std::atomic<int> g_hw_veh_caught{0};

i32 TRT_GUEST_ABI veh_hw_recover(void* ep) {
  if (!ep) return 0;
  const u8* rec = *(const u8* const*)ep;
  u8* ctx = *(u8* const*)((const u8*)ep + 8);
  const u32 code = *(const u32*)(rec + 0);
  const u64 addr = *(const u64*)(rec + 16);
  if (code == 0xC0000005u && addr == g_hw_expected_pc) {
    *(u64*)(ctx + 0x80) = (u64)(uintptr_t)&g_hw_safe_word;  // Rcx
    g_hw_veh_caught.fetch_add(1, std::memory_order_relaxed);
    return -1;  // EXCEPTION_CONTINUE_EXECUTION
  }
  return 0;
}

void test_hw_segv_handler(int sig, siginfo_t* si, void* uc) {
  if (trt_dispatch_hw_fault(sig, si, uc)) return;
  _exit(127);
}

std::atomic<u32> g_host_log_last{0};
void TRT_GUEST_ABI test_host_log_u32(u32 tag, u32 value) {
  (void)tag;
  g_host_log_last.store(value, std::memory_order_release);
}

}  // namespace

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  printf("tiramisu wave1 runtime verification suite\n");

  const trt_backend be{mmap_alloc, mmap_protect, mmap_free, quiet_log};
  char tmpdir[] = "/tmp/tiramisu_w1_XXXXXX";
  CHECK(mkdtemp(tmpdir) != nullptr, "create temp workspace");
  trt_bridge_boot(&be);
  trt_bridge_register_kernel32();
  {
    extern int trt_bridge_register_user32(void);
    trt_bridge_register_user32();
  }
  trt_vfs_mount("c:", tmpdir);

  // ---- Wave 1A.1: Large sparse file (>64 MiB) streamed in 4 KB window ----
  std::string big_host = std::string(tmpdir) + "/big_pak.bin";
  {
    FILE* f = fopen(big_host.c_str(), "wb");
    CHECK(f != nullptr, "open large sparse test archive");
    // Write a marker at 64 MiB + 512 using fseeko (sparse on Linux ext4/tmpfs)
    const char magic[] = "STREAMING_PAK_64MB_MARKER";
    fseeko(f, (off_t)(64ull << 20) + 512, SEEK_SET);
    fwrite(magic, 1, sizeof(magic) - 1, f);
    // Ensure total size > 64 MiB + 4096
    fseeko(f, (off_t)(64ull << 20) + 8192, SEEK_SET);
    fputc(0, f);
    fclose(f);
  }

  {
    u16 wpath[128];
    make_u16("C:\\big_pak.bin", wpath, 128);
    void* fh = trt_CreateFileW(wpath, 0xC0000000u /*RW*/, 0, nullptr,
                               3 /*OPEN_EXISTING*/, 0, nullptr);
    CHECK(fh != (void*)(uintptr_t)-1, "CreateFileW opens >64 MiB archive");

    u64 hmap = trt_CreateFileMappingW((u64)(uintptr_t)fh, nullptr,
                                      0x04 /*PAGE_READWRITE*/, 0, 0, nullptr);
    CHECK(hmap != 0, "CreateFileMappingW succeeds on large file");

    // Classic Win32 pattern: close file handle immediately, keep mapping open!
    trt_CloseHandle(fh);

    u32 maps = 0, views = 0;
    u64 view_bytes = 0;
    trt_mem_stats(&maps, &views, &view_bytes);
    CHECK(maps >= 1 && views == 0 && view_bytes == 0,
          "CreateFileMappingW uses 0 view bytes before MapViewOfFile (no full slurp)");

    // Map ONLY a 4096-byte window starting at 64 MiB
    const u64 off = 64ull << 20;
    u8* view = (u8*)trt_MapViewOfFile(hmap, 0x06 /*R|W*/, (u32)(off >> 32),
                                      (u32)off, 4096);
    CHECK(view != nullptr, "MapViewOfFile(offset=64MiB, bytes=4096) succeeds");
    CHECK(memcmp(view + 512, "STREAMING_PAK_64MB_MARKER", 25) == 0,
          "Window reads exact bytes at 64 MiB offset after CloseHandle(hFile)");

    trt_mem_stats(&maps, &views, &view_bytes);
    CHECK(views == 1 && view_bytes == 4096,
          "Mapped footprint equals window size (4096 B), not file size (64 MiB+)");

    // Write through view + FlushViewOfFile -> verify on host file
    memcpy(view + 1024, "DIRTY_FLUSH_OK", 14);
    CHECK(trt_FlushViewOfFile(view + 1024, 14) == 1,
          "FlushViewOfFile succeeds on interior pointer");
    CHECK(trt_UnmapViewOfFile(view) == 1, "UnmapViewOfFile succeeds");

    trt_mem_stats(&maps, &views, &view_bytes);
    CHECK(views == 0 && view_bytes == 0,
          "UnmapViewOfFile immediately releases view RAM");

    trt_CloseHandle((void*)(uintptr_t)hmap);
    trt_mem_stats(&maps, &views, &view_bytes);
    CHECK(maps == 0, "CloseHandle(hMap) drops mapping & retains zero leaks");

    // Confirm flushed bytes landed on disk
    FILE* vf = fopen(big_host.c_str(), "rb");
    char verify_buf[16] = {};
    fseeko(vf, (off_t)off + 1024, SEEK_SET);
    fread(verify_buf, 1, 14, vf);
    fclose(vf);
    CHECK(memcmp(verify_buf, "DIRTY_FLUSH_OK", 14) == 0,
          "FlushViewOfFile persisted changes to disk at 64 MiB offset");
  }

  // ---- Wave 1A.2: Named mappings + OpenFileMappingW + >128 mappings ----
  {
    u16 name1[64], name2[64];
    make_u16("Global\\SharedAudioShm", name1, 64);
    make_u16("global\\sharedaudioshm", name2, 64);

    u64 h1 = trt_CreateFileMappingW((u64)-1, nullptr, 0x04, 0, 8192, name1);
    CHECK(h1 != 0 && trt_GetLastError() == 0,
          "CreateFileMappingW creates named anonymous mapping");

    u64 h2 = trt_CreateFileMappingW((u64)-1, nullptr, 0x04, 0, 8192, name2);
    CHECK(h2 != 0 && h2 != h1 && trt_GetLastError() == 183 /*ERROR_ALREADY_EXISTS*/,
          "CreateFileMappingW re-open returns distinct handle + ERROR_ALREADY_EXISTS");

    u64 h3 = trt_OpenFileMappingW(0xF001Fu, 0, name1);
    CHECK(h3 != 0, "OpenFileMappingW finds named mapping case-insensitively");

    u8* v1 = (u8*)trt_MapViewOfFile(h1, 0x02 /*WRITE*/, 0, 0, 4096);
    CHECK(v1 != nullptr, "MapViewOfFile on named mapping handle 1 succeeds");
    strcpy((char*)v1, "SHARED_IPC_STATE");
    CHECK(trt_FlushViewOfFile(v1, 32) == 1, "FlushViewOfFile on anon view works");
    trt_UnmapViewOfFile(v1);

    u8* v3 = (u8*)trt_MapViewOfFile(h3, 0x04 /*READ*/, 0, 0, 4096);
    CHECK(v3 != nullptr && strcmp((char*)v3, "SHARED_IPC_STATE") == 0,
          "OpenFileMappingW sees bytes flushed by creator handle");
    trt_UnmapViewOfFile(v3);

    trt_CloseHandle((void*)(uintptr_t)h1);
    trt_CloseHandle((void*)(uintptr_t)h2);
    trt_CloseHandle((void*)(uintptr_t)h3);
  }

  // ---- Wave 1A.3: Uncapped (>128) simultaneous mappings + open files ----
  {
    u64 hs[140] = {};
    int ok = 0;
    for (int i = 0; i < 140; ++i) {
      hs[i] = trt_CreateFileMappingW((u64)-1, nullptr, 0x04, 0, 4096, nullptr);
      if (hs[i]) ++ok;
    }
    CHECK(ok == 140, "140 simultaneous mappings exceed legacy 64-slot limit");
    for (int i = 0; i < 140; ++i) trt_CloseHandle((void*)(uintptr_t)hs[i]);

    u32 maps = 0, views = 0;
    u64 view_bytes = 0;
    trt_mem_stats(&maps, &views, &view_bytes);
    CHECK(maps == 0 && views == 0 && view_bytes == 0,
          "All 140 mappings reclaimed cleanly after CloseHandle");
  }

  // ---- Wave 1B.1: Multi-page fail-visible thunks (>128) ----
  {
    void* stubs[150] = {};
    int unique_ok = 0;
    for (int i = 0; i < 150; ++i) {
      char sym[32];
      snprintf(sym, sizeof(sym), "MissingApi_%d", i);
      stubs[i] = trt_thunk_make(&be, "STUB", "wave1mod.dll", sym);
      if (stubs[i]) ++unique_ok;
    }
    CHECK(unique_ok == 150 && stubs[0] != stubs[149] && stubs[64] != stubs[0],
          "150 distinct executable thunks span multiple W^X 4KB pages cleanly");

    typedef u64 (TRT_GUEST_ABI* stub_fn)(void);
    const u64 r149 = ((stub_fn)stubs[149])();
    CHECK(r149 == 0, "Executing 150th W^X thunk succeeds and returns 0");
  }

  // ---- Wave 1B.2: Per-thread .tls isolation + Suspend/GetContext/Resume ----
  {
    const u32 init_val = 0xDEADBEEFu;
    u32 tls_idx = 0;
    CHECK(trt_guest_tls_register((const u8*)&init_val, sizeof(init_val), 64,
                                 &tls_idx) == 0,
          "trt_guest_tls_register registers dynamic .tls template");

    TlsWorkerArgs w1{tls_idx, 0x11111111u, 0, 0,
                     trt_CreateEventW(nullptr, true, false, nullptr),
                     trt_CreateEventW(nullptr, true, false, nullptr)};
    TlsWorkerArgs w2{tls_idx, 0x22222222u, 0, 0,
                     trt_CreateEventW(nullptr, true, false, nullptr),
                     trt_CreateEventW(nullptr, true, false, nullptr)};
    const u64 th1 = trt_CreateThread(nullptr, nullptr, (void*)&guest_tls_worker,
                                     &w1, 0, nullptr);
    const u64 th2 = trt_CreateThread(nullptr, nullptr, (void*)&guest_tls_worker,
                                     &w2, 0, nullptr);
    CHECK(th1 && th2, "CreateThread spawns two guest TLS worker threads");
    CHECK(trt_WaitForSingleObject(w1.ready_ev, 5000) == 0 &&
              trt_WaitForSingleObject(w2.ready_ev, 5000) == 0,
          "Both worker threads signaled TLS readiness");
    trt_SetEvent(w1.proceed_ev);
    trt_SetEvent(w2.proceed_ev);
    const u64 ths[2] = {th1, th2};
    CHECK(trt_WaitForMultipleObjects(2, ths, true, 5000) == 0,
          "WaitForMultipleObjects(wait_all=true) joins both threads");
    CHECK(w1.read_initial == 0xDEADBEEFu && w2.read_initial == 0xDEADBEEFu &&
              w1.read_after == 0x11111111u && w2.read_after == 0x22222222u,
          "Per-thread .tls slots initialize from template and stay isolated");
    trt_CloseHandle((void*)(uintptr_t)th1);
    trt_CloseHandle((void*)(uintptr_t)th2);
    trt_CloseHandle((void*)(uintptr_t)w1.ready_ev);
    trt_CloseHandle((void*)(uintptr_t)w1.proceed_ev);
    trt_CloseHandle((void*)(uintptr_t)w2.ready_ev);
    trt_CloseHandle((void*)(uintptr_t)w2.proceed_ev);

    // CREATE_SUSPENDED + GetThreadContext + SetThreadContext
    const u64 sth = trt_CreateThread(
        nullptr, nullptr, (void*)&guest_spin_worker, nullptr, 0x4u, nullptr);
    u8 ctx[1232] = {};
    *(u32*)(ctx + 0x30) = 0x00100003u;  // CONTEXT_CONTROL | CONTEXT_INTEGER
    CHECK(sth != 0 && trt_GetThreadContext(sth, ctx) == 1 &&
              *(u64*)(ctx + 0xF8) == (u64)(uintptr_t)&guest_spin_worker,
          "CREATE_SUSPENDED thread GetThreadContext reports start RIP");
    *(u64*)(ctx + 0xF8) = (u64)(uintptr_t)&guest_suspended_override;
    *(u64*)(ctx + 0x80) = 55u;  // Rcx
    CHECK(trt_SetThreadContext(sth, ctx) == 1 && trt_ResumeThread(sth) == 1 &&
              trt_WaitForSingleObject(sth, 5000) == 0,
          "SetThreadContext before ResumeThread redirects suspended thread");
    trt_CloseHandle((void*)(uintptr_t)sth);

    // Live worker SuspendThread + GetThreadContext + ResumeThread
    g_susp_spin.store(true, std::memory_order_release);
    g_susp_entered.store(false, std::memory_order_release);
    const u64 lth = trt_CreateThread(
        nullptr, nullptr, (void*)&guest_spin_worker, nullptr, 0, nullptr);
    while (!g_susp_entered.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    const u32 prev_susp = trt_SuspendThread(lth);
    u8 lctx[1232] = {};
    *(u32*)(lctx + 0x30) = 0x0010000Fu;
    CHECK(prev_susp == 0 && trt_GetThreadContext(lth, lctx) == 1 &&
              *(u64*)(lctx + 0xF8) != 0,
          "SuspendThread + GetThreadContext captures live worker RIP via SIGUSR2");
    g_susp_spin.store(false, std::memory_order_release);
    CHECK(trt_ResumeThread(lth) == 1 &&
              trt_WaitForSingleObject(lth, 5000) == 0,
          "ResumeThread wakes suspended worker thread cleanly");
    trt_CloseHandle((void*)(uintptr_t)lth);
  }

  // ---- Wave 1B.3: Ordered VEH chain (>32 handlers, first=1 vs 0) ----
  {
    std::vector<void*> dummies;
    for (int i = 0; i < 36; ++i) {
      dummies.push_back(
          trt_AddVectoredExceptionHandler(0, (void*)&veh_mid));
    }
    void* h_mid = trt_AddVectoredExceptionHandler(0, (void*)&veh_mid);
    void* h_tail = trt_AddVectoredExceptionHandler(0, (void*)&veh_tail_catch);
    void* h_head = trt_AddVectoredExceptionHandler(1, (void*)&veh_head);
    CHECK(dummies.size() == 36 && h_head && h_mid && h_tail,
          ">32 simultaneous VEH registrations succeed");
    g_veh_trace.clear();
    trt_RaiseException(0xE0000001u, 0, 0, nullptr);
    CHECK(!g_veh_trace.empty() && g_veh_trace.front() == 1 &&
              g_veh_trace.back() == 3 && g_veh_trace.size() == 39,
          "VEH order honors first=1 prepend and first=0 append + catch");
    trt_RemoveVectoredExceptionHandler(h_head);
    trt_RemoveVectoredExceptionHandler(h_mid);
    trt_RemoveVectoredExceptionHandler(h_tail);
    for (void* h : dummies) trt_RemoveVectoredExceptionHandler(h);
  }

  // ---- Wave 1B.4: Uncapped (>16) Windows + SetTimer/WM_TIMER + IOCP ----
  {
    void* wins[24] = {};
    bool all_unique = true;
    for (int i = 0; i < 24; ++i) {
      wins[i] = trt_CreateWindowExW(0, L"STATIC", L"W", 0, 0, 0, 64, 64,
                                    nullptr, nullptr, nullptr, nullptr);
      if (!wins[i]) all_unique = false;
      for (int j = 0; j < i; ++j) {
        if (wins[j] == wins[i]) all_unique = false;
      }
    }
    CHECK(all_unique, "24 distinct Win32 HWNDs created without 16-slot reuse");

    g_timer_ticks.store(0, std::memory_order_release);
    const u64 tid = trt_SetTimer(wins[20], 42u, 10u, (void*)&guest_timer_cb);
    usleep(30000);
    u8 msg[48] = {};
    int timer_msgs = 0;
    while (trt_PeekMessageW(msg, nullptr, 0, 0, 1)) {
      if (*(u32*)(msg + 8) == 0x0113u /*WM_TIMER*/) {
        ++timer_msgs;
        trt_DispatchMessageW(msg);
      }
    }
    CHECK(tid == 42u && timer_msgs >= 1 && g_timer_ticks.load() >= 1,
          "SetTimer fires WM_TIMER through PeekMessageW/DispatchMessageW");
    trt_KillTimer(wins[20], 42u);
    for (int i = 0; i < 24; ++i) trt_DestroyWindow(wins[i]);

    // IOCP + OVERLAPPED ReadFile
    u16 fname[64];
    make_u16("c:\\iocp_wave1.bin", fname, 64);
    void* fh = trt_CreateFileW(fname, 0xC0000000u, 3, nullptr, 2, 0x80, nullptr);
    u32 nw = 0;
    trt_WriteFile(fh, "IOCP_PAYLOAD_OK!", 16, &nw, nullptr);
    trt_CloseHandle(fh);

    fh = trt_CreateFileW(fname, 0x80000000u, 3, nullptr, 3, 0x40000080u, nullptr);
    const u64 iocp = trt_CreateIoCompletionPort(fh, 0, 0xCAFEBABEu, 1);
    u64 ovl[4] = {};
    ovl[3] = trt_CreateEventW(nullptr, true, false, nullptr);
    char rbuf[32] = {};
    u32 nr = 0;
    trt_ReadFile(fh, rbuf, 16, &nr, ovl);
    u32 ov_bytes = 0;
    u32 q_bytes = 0;
    u64 q_key = 0;
    void* q_ovl = nullptr;
    CHECK(trt_WaitForSingleObject(ovl[3], 100) == 0 &&
              trt_GetOverlappedResult(fh, ovl, &ov_bytes, 1) == 1 &&
              ov_bytes == 16 &&
              trt_GetQueuedCompletionStatus(iocp, &q_bytes, &q_key, &q_ovl, 500) == 1 &&
              q_bytes == 16 && q_key == 0xCAFEBABEu && q_ovl == ovl &&
              memcmp(rbuf, "IOCP_PAYLOAD_OK!", 16) == 0,
          "OVERLAPPED ReadFile signals hEvent + posts IOCP completion packet");
    trt_CloseHandle((void*)(uintptr_t)ovl[3]);
    trt_CloseHandle((void*)(uintptr_t)iocp);
    trt_CloseHandle(fh);
  }

  // ---- Wave 1B.5: AppModel family + CreateProcessW crash-handler bypass ----
  {
    CHECK(trt_module_query("api-ms-win-appmodel-runtime-l1-1-2.dll",
                           "GetCurrentPackageFullName") != 0,
          "api-ms-win-appmodel-runtime routes to kernel32 exports");
    u32 plen = 64;
    u16 pbuf[64] = {};
    CHECK(trt_GetCurrentPackageFullName(&plen, pbuf) == 15700u /*APPMODEL_ERROR_NO_PACKAGE*/,
          "GetCurrentPackageFullName returns APPMODEL_ERROR_NO_PACKAGE (15700)");

    u16 crash_exe[64], unknown_exe[64];
    make_u16("UnityCrashHandler64.exe", crash_exe, 64);
    make_u16("notepad.exe", unknown_exe, 64);
    u8 pi[24] = {};
    u8 si[104] = {};
    *(u32*)si = sizeof(si);
    const int crash_ok = trt_CreateProcessW(
        crash_exe, nullptr, nullptr, nullptr, 0, 0, nullptr, nullptr, si, pi);
    const u64 hproc = *(u64*)pi;
    CHECK(crash_ok == 1 && hproc != 0 &&
              trt_WaitForSingleObject(hproc, 0) == 0,
          "CreateProcessW treats UnityCrashHandler64.exe as clean helper");
    if (hproc) trt_CloseHandle((void*)(uintptr_t)hproc);
    if (*(u64*)(pi + 8)) trt_CloseHandle((void*)(uintptr_t)*(u64*)(pi + 8));

    memset(pi, 0, sizeof(pi));
    trt_SetLastError(0);
    const int unk_ok = trt_CreateProcessW(
        unknown_exe, nullptr, nullptr, nullptr, 0, 0, nullptr, nullptr, si, pi);
    CHECK(unk_ok == 0 && trt_GetLastError() == 5u && *(u64*)pi == 0,
          "CreateProcessW fails unknown exe fail-visibly (ERROR_ACCESS_DENIED)");
  }

  // ---- Wave 1B.6: WaitOnAddress exact hash-collision wakeup ----
  {
    alignas(8) u64 collide[65] = {};
    collide[0] = 1;
    collide[64] = 1;
    std::atomic<bool> a_ready{false}, b_ready{false};
    std::atomic<bool> a_done{false}, b_done{false};
    std::thread ta([&] {
      u64 cmp = 1;
      a_ready.store(true, std::memory_order_release);
      trt_WaitOnAddress(&collide[0], &cmp, sizeof(u64), 3000);
      a_done.store(true, std::memory_order_release);
    });
    std::thread tb([&] {
      u64 cmp = 1;
      b_ready.store(true, std::memory_order_release);
      trt_WaitOnAddress(&collide[64], &cmp, sizeof(u64), 3000);
      b_done.store(true, std::memory_order_release);
    });
    while (!a_ready.load(std::memory_order_acquire) ||
           !b_ready.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    collide[64] = 2;
    trt_WakeByAddressSingle(&collide[64]);
    tb.join();
    const bool targeted = b_done.load(std::memory_order_acquire) &&
                          !a_done.load(std::memory_order_acquire);
    collide[0] = 2;
    trt_WakeByAddressAll(&collide[0]);
    ta.join();
    CHECK(targeted && a_done.load(std::memory_order_acquire),
          "WaitOnAddress wakes target address under 64-bucket hash collision");
  }

  // ---- Wave 1B.7: trt_dispatch_hw_fault + guest VEH recovery ----
  {
    u8* jit = (u8*)trt_VirtualAlloc(nullptr, 4096, 0x3000u, 0x04u);
    CHECK(jit != nullptr, "VirtualAlloc RW JIT page for fault recovery test");
    if (jit) {
      // 48 31 c9 = xor rcx, rcx; 8b 01 = mov eax, [rcx]; c3 = ret
      static const u8 kCode[] = {0x48, 0x31, 0xC9, 0x8B, 0x01, 0xC3};
      memcpy(jit, kCode, sizeof(kCode));
      u32 oldp = 0;
      CHECK(trt_VirtualProtect(jit, 4096, 0x20u /*PAGE_EXECUTE_READ*/, &oldp) == 1,
            "VirtualProtect JIT page to PAGE_EXECUTE_READ");
      g_hw_expected_pc = (u64)(uintptr_t)(jit + 3);
      g_hw_veh_caught.store(0, std::memory_order_release);
      void* hveh = trt_AddVectoredExceptionHandler(1, (void*)&veh_hw_recover);
      struct sigaction sa = {}, old_sa = {};
      sigemptyset(&sa.sa_mask);
      sa.sa_flags = SA_SIGINFO;
      sa.sa_sigaction = test_hw_segv_handler;
      sigaction(SIGSEGV, &sa, &old_sa);
      using jit_fn_t = u32 (*)(void);
      const u32 res = ((jit_fn_t)jit)();
      sigaction(SIGSEGV, &old_sa, nullptr);
      trt_RemoveVectoredExceptionHandler(hveh);
      trt_VirtualFree(jit, 0, 0x8000u);
      CHECK(res == 0x12345678u && g_hw_veh_caught.load() == 1,
            "trt_dispatch_hw_fault recovers NULL read via guest VEH RCX fixup");
    }
  }

  // ---- Wave 1C.1: SetDllDirectoryW, AddDllDirectory, extensionless LoadLibraryW, AppPolicy ----
  {
    trt_runtime_init(&be, tmpdir);
    u32 term_method = 99;
    CHECK(trt_AppPolicyGetProcessTerminationMethod((void*)(uintptr_t)-1,
                                                   &term_method) == 0 &&
              term_method == 0u,
          "AppPolicyGetProcessTerminationMethod -> S_OK, ExitProcess(0)");
    CHECK(trt_SetDefaultDllDirectories(0x1000u) == 1,
          "SetDefaultDllDirectories returns TRUE");

    std::string pdir1 = std::string(tmpdir) + "/w1_plug1";
    std::string pdir2 = std::string(tmpdir) + "/w1_plug2";
    mkdir(pdir1.c_str(), 0755);
    mkdir(pdir2.c_str(), 0755);

    u16 wdir1[128], wdir2[128], wget[128]{};
    make_u16("C:\\w1_plug1", wdir1, 128);
    make_u16("C:\\w1_plug2", wdir2, 128);
    CHECK(trt_SetDllDirectoryW(wdir1) == 1, "SetDllDirectoryW succeeds");
    const u32 dlen = trt_GetDllDirectoryW(128, wget);
    CHECK(dlen == 11u && wget[0] == 'C' && wget[2] == '\\' && wget[3] == 'w',
          "GetDllDirectoryW returns normalized guest directory");

    // Copy tests/out/guestdll.dll into both plugin dirs under distinct names
    FILE* fsrc = fopen("tests/out/guestdll.dll", "rb");
    bool copied = false;
    if (fsrc) {
      fseek(fsrc, 0, SEEK_END);
      long sz = ftell(fsrc);
      fseek(fsrc, 0, SEEK_SET);
      if (sz > 0) {
        std::vector<u8> bytes((size_t)sz);
        fread(bytes.data(), 1, (size_t)sz, fsrc);
        std::string d1 = pdir1 + "/guestdll_sub.dll";
        std::string d2 = pdir2 + "/guestdll_add.dll";
        FILE* f1 = fopen(d1.c_str(), "wb");
        FILE* f2 = fopen(d2.c_str(), "wb");
        if (f1 && f2) {
          fwrite(bytes.data(), 1, bytes.size(), f1);
          fwrite(bytes.data(), 1, bytes.size(), f2);
          copied = true;
        }
        if (f1) fclose(f1);
        if (f2) fclose(f2);
      }
      fclose(fsrc);
    }
    CHECK(copied, "prepared PE64 DLL fixtures in C:\\w1_plug1 and C:\\w1_plug2");

    static const trt_named_fn k_host_fns[] = {
        {"TiramisuHostLogU32", (void*)&test_host_log_u32},
    };
    trt_module_register("tiramisu_host.dll", TRT_ROUTE_BUILTIN, k_host_fns, 1);

    // Clear SetDllDirectoryW first -> extensionless load must fail
    trt_SetDllDirectoryW(nullptr);
    u16 wsub_extless[64], wadd_extless[64];
    make_u16("guestdll_sub", wsub_extless, 64);
    make_u16("guestdll_add", wadd_extless, 64);
    CHECK(trt_LoadLibraryW(wsub_extless) == nullptr,
          "LoadLibraryW fails extensionless miss before SetDllDirectoryW");

    // Now set SetDllDirectoryW("C:\\w1_plug1") and load without .dll suffix
    CHECK(trt_SetDllDirectoryW(wdir1) == 1, "re-set SetDllDirectoryW");
    trt_set_image_code_run(1);
    g_host_log_last.store(0, std::memory_order_release);
    void* m1 = trt_LoadLibraryW(wsub_extless);
    CHECK(m1 != nullptr && g_host_log_last.load(std::memory_order_acquire) == 0x1111u,
          "LoadLibraryW loads extensionless PE64 DLL from SetDllDirectoryW");
    if (m1) {
      using add_fn_t = u32(TRT_GUEST_ABI*)(u32, u32);
      auto p_add = (add_fn_t)trt_GetProcAddress(m1, "dll_add");
      CHECK(p_add != nullptr && p_add(19u, 23u) == 42u &&
                g_host_log_last.load(std::memory_order_acquire) == 42u,
            "GetProcAddress + execute PE64 DLL export works via SetDllDirectoryW");
      trt_FreeLibrary(m1);
    }

    // AddDllDirectory + RemoveDllDirectory
    void* cookie = trt_AddDllDirectory(wdir2);
    CHECK(cookie != nullptr, "AddDllDirectory returns non-null cookie");
    void* m2 = trt_LoadLibraryW(wadd_extless);
    CHECK(m2 != nullptr, "LoadLibraryW finds extensionless DLL in AddDllDirectory");
    if (m2) trt_FreeLibrary(m2);
    CHECK(trt_RemoveDllDirectory(cookie) == 1 && trt_RemoveDllDirectory(cookie) == 0,
          "RemoveDllDirectory removes entry and rejects duplicate remove");
    trt_set_image_code_run(0);
    trt_SetDllDirectoryW(nullptr);
  }

  // ---- Wave 1C.2: SteamClient + non-null vtables + RemoteStorage + SteamInput + DRM honesty ----
  {
    auto sq = [](const char* sym) -> void* {
      return trt_resolve_module(nullptr, "steam_api64.dll", sym, 0);
    };
    using fn0_i = int(TRT_GUEST_ABI*)(void);
    using fn0_p = void*(TRT_GUEST_ABI*)(void);
    using fn1_cp = void*(TRT_GUEST_ABI*)(const char*);
    using fn_getiface = void*(TRT_GUEST_ABI*)(void*, int, int, const char*);
    using fn_getgeneric = void*(TRT_GUEST_ABI*)(void*, int, int, const char*);
    using fn_bsub = int(TRT_GUEST_ABI*)(void*, u32);

    auto pInit = (fn0_i)sq("SteamAPI_Init");
    auto pClient = (fn0_p)sq("SteamClient");
    auto pCreateIf = (fn1_cp)sq("SteamInternal_CreateInterface");
    auto pGetUser = (fn_getiface)sq("SteamAPI_ISteamClient_GetISteamUser");
    auto pGetFriends = (fn_getiface)sq("SteamAPI_ISteamClient_GetISteamFriends");
    auto pGetApps = (fn_getiface)sq("SteamAPI_ISteamClient_GetISteamApps");
    auto pGetRS = (fn_getiface)sq("SteamAPI_ISteamClient_GetISteamRemoteStorage");
    auto pGetInput = (fn_getiface)sq("SteamAPI_ISteamClient_GetISteamInput");
    auto pGetGen = (fn_getgeneric)sq("SteamAPI_ISteamClient_GetISteamGenericInterface");
    auto pBSub = (fn_bsub)sq("SteamAPI_ISteamApps_BIsSubscribedApp");

    CHECK(pInit && pInit() == 0, "SteamAPI_Init reports unavailable client");
    void* client = pClient ? pClient() : nullptr;
    void* client2 = pCreateIf ? pCreateIf("SteamClient020") : nullptr;
    CHECK(client != nullptr && client == client2,
          "SteamClient() and SteamInternal_CreateInterface(\"SteamClient020\") match");

    void* s_user = (client && pGetUser) ? pGetUser(client, 1, 1, "SteamUser021") : nullptr;
    void* s_friends = (client && pGetFriends) ? pGetFriends(client, 1, 1, "SteamFriends018") : nullptr;
    void* s_apps = (client && pGetApps) ? pGetApps(client, 1, 1, "SteamApps008") : nullptr;
    void* s_rs = (client && pGetRS) ? pGetRS(client, 1, 1, "SteamRemoteStorage016") : nullptr;
    void* s_input = (client && pGetInput) ? pGetInput(client, 1, 1, "SteamInput006") : nullptr;
    void* s_stats = (client && pGetGen) ? pGetGen(client, 1, 1, "SteamUserStats013") : nullptr;
    void* s_unk = pCreateIf ? pCreateIf("SteamFutureCustom999") : nullptr;

    auto vtbl_ok = [](void* obj) -> bool {
      if (!obj) return false;
      const void* const* vt = *(const void* const* const*)obj;
      return vt != nullptr && vt[0] != nullptr;
    };
    CHECK(vtbl_ok(client) && vtbl_ok(s_user) && vtbl_ok(s_friends) &&
              vtbl_ok(s_apps) && vtbl_ok(s_rs) && vtbl_ok(s_input) &&
              vtbl_ok(s_stats) && vtbl_ok(s_unk),
          "All Steamworks interface singletons expose non-null safe vtables");

    // Calling virtual slot on generic interface must not jump to NULL
    using vt0_u64 = u64(TRT_GUEST_ABI*)(void*);
    const void* const* stats_vt = *(const void* const* const*)s_stats;
    CHECK(((vt0_u64)stats_vt[0])(s_stats) == 0,
          "Calling SteamUserStats vtable slot 0 returns safely without NULL deref");

    // DRM / Ownership honesty preserved
    const void* const* apps_vt = *(const void* const* const*)s_apps;
    CHECK(pBSub && pBSub(s_apps, 480u) == 0 &&
              ((fn_bsub)apps_vt[6])(s_apps, 480u) == 0,
          "Steam BIsSubscribedApp remains honest 0 via both flat and vtable paths");

    // ISteamRemoteStorage VFS cloud-save persistence + traversal guard
    using fn_fw = int(TRT_GUEST_ABI*)(void*, const char*, const void*, i32);
    using fn_fr = i32(TRT_GUEST_ABI*)(void*, const char*, void*, i32);
    using fn_fe = int(TRT_GUEST_ABI*)(void*, const char*);
    using fn_gsz = i32(TRT_GUEST_ABI*)(void*, const char*);
    using fn_cnt = i32(TRT_GUEST_ABI*)(void*);
    using fn_fnm = const char*(TRT_GUEST_ABI*)(void*, int, int*);
    auto pFW = (fn_fw)sq("SteamAPI_ISteamRemoteStorage_FileWrite");
    auto pFR = (fn_fr)sq("SteamAPI_ISteamRemoteStorage_FileRead");
    auto pFE = (fn_fe)sq("SteamAPI_ISteamRemoteStorage_FileExists");
    auto pFD = (fn_fe)sq("SteamAPI_ISteamRemoteStorage_FileDelete");
    auto pGSz = (fn_gsz)sq("SteamAPI_ISteamRemoteStorage_GetFileSize");
    auto pCnt = (fn_cnt)sq("SteamAPI_ISteamRemoteStorage_GetFileCount");
    auto pFnm = (fn_fnm)sq("SteamAPI_ISteamRemoteStorage_GetFileNameAndSize");

    const char payload[] = "SAVE_BLOB_OK";
    char rd[32] = {};
    int fsz = 0;
    CHECK(pFW && pFW(s_rs, "saves/slot1.bin", payload, 12) == 1 &&
              pFE && pFE(s_rs, "saves/slot1.bin") == 1 &&
              pGSz && pGSz(s_rs, "saves/slot1.bin") == 12 &&
              pFR && pFR(s_rs, "saves/slot1.bin", rd, sizeof(rd)) == 12 &&
              memcmp(rd, payload, 12) == 0 &&
              pCnt && pCnt(s_rs) >= 1 &&
              pFnm && strcmp(pFnm(s_rs, 0, &fsz), "saves\\slot1.bin") == 0 &&
              fsz == 12,
          "ISteamRemoteStorage persists read/write/size/index via VFS");
    CHECK(pFW(s_rs, "../escape.bin", payload, 4) == 0 &&
              pFD && pFD(s_rs, "saves/slot1.bin") == 1 &&
              pFE(s_rs, "saves/slot1.bin") == 0,
          "ISteamRemoteStorage blocks path traversal and supports FileDelete");

    // ISteamInput backed by 4-slot XInput
    using fn_icnt = int(TRT_GUEST_ABI*)(void*, u64*);
    using fn_igpi = int(TRT_GUEST_ABI*)(void*, u64);
    using fn_itype = int(TRT_GUEST_ABI*)(void*, u64);
    using fn_ivib = void(TRT_GUEST_ABI*)(void*, u64, u16, u16);
    auto pICnt = (fn_icnt)sq("SteamAPI_ISteamInput_GetConnectedControllers");
    auto pIGpi = (fn_igpi)sq("SteamAPI_ISteamInput_GetGamepadIndexForController");
    auto pIType = (fn_itype)sq("SteamAPI_ISteamInput_GetInputTypeForHandle");
    auto pIVib = (fn_ivib)sq("SteamAPI_ISteamInput_TriggerVibration");

    trt_input_pad_state_n(0, true, 0, 0, 0, 0, 0, 0, 0);
    trt_input_pad_state_n(1, false, 0, 0, 0, 0, 0, 0, 0);
    trt_input_pad_state_n(2, true, 0, 0, 0, 0, 0, 0, 0);
    trt_input_pad_state_n(3, false, 0, 0, 0, 0, 0, 0, 0);
    u64 ihandles[16] = {};
    const int nctrl = pICnt ? pICnt(s_input, ihandles) : -1;
    u16 vl = 0, vr = 0;
    if (nctrl == 2 && pIVib) pIVib(s_input, ihandles[1], 7777u, 8888u);
    CHECK(nctrl == 2 && pIGpi && pIGpi(s_input, ihandles[0]) == 0 &&
              pIGpi(s_input, ihandles[1]) == 2 &&
              pIType && pIType(s_input, ihandles[0]) == 3 &&
              trt_pad_get_vibration(2, &vl, &vr) && vl == 7777u && vr == 8888u,
          "ISteamInput enumerates XInput controllers and drives per-slot vibration");
  }

  // ---- 6. Fibers, Vista Threadpool, CPU Topology & System Telemetry ----
  {
    auto kfn = [&](const char* mod, const char* sym) -> void* {
      return trt_resolve_module(nullptr, mod, sym, 0);
    };

    // 6A. Fibers (ConvertThreadToFiber / CreateFiber / SwitchToFiber / GetFiberData)
    using ConvertToFiber_t = void*(TRT_STDCALL*)(void*);
    using ConvertToThread_t = int(TRT_STDCALL*)(void);
    using CreateFiber_t = void*(TRT_STDCALL*)(u64, void*, void*);
    using DeleteFiber_t = void(TRT_STDCALL*)(void*);
    using SwitchToFiber_t = void(TRT_STDCALL*)(void*);
    using IsThreadAFiber_t = int(TRT_STDCALL*)(void);

    auto pConvToFib = (ConvertToFiber_t)kfn("kernel32.dll", "ConvertThreadToFiber");
    auto pConvToThr = (ConvertToThread_t)kfn("kernel32.dll", "ConvertFiberToThread");
    auto pCreateFib = (CreateFiber_t)kfn("kernel32.dll", "CreateFiber");
    auto pDeleteFib = (DeleteFiber_t)kfn("kernel32.dll", "DeleteFiber");
    auto pSwitchFib = (SwitchToFiber_t)kfn("kernel32.dll", "SwitchToFiber");
    auto pIsFib = (IsThreadAFiber_t)kfn("kernel32.dll", "IsThreadAFiber");

    struct fiber_ctx {
      void* main_fib;
      SwitchToFiber_t switch_fn;
      int steps;
      uintptr_t magic;
    };
    auto fiber_proc = +[](void* param) TRT_STDCALL -> void {
      auto* fc = (fiber_ctx*)param;
      fc->magic = 0xF1BE2026u;
      fc->steps += 1;
      fc->switch_fn(fc->main_fib);
      fc->steps += 10;
      fc->switch_fn(fc->main_fib);
    };

    void* main_fib = pConvToFib ? pConvToFib((void*)0x1111) : nullptr;
    fiber_ctx fctx{main_fib, pSwitchFib, 0, 0};
    void* child_fib = (main_fib && pCreateFib)
                          ? pCreateFib(65536, (void*)fiber_proc, &fctx)
                          : nullptr;
    const int is_fib_while = pIsFib ? pIsFib() : 0;
    if (child_fib && pSwitchFib) {
      pSwitchFib(child_fib);
      pSwitchFib(child_fib);
    }
    if (child_fib && pDeleteFib) pDeleteFib(child_fib);
    const int conv_back = pConvToThr ? pConvToThr() : 0;
    const int is_fib_after = pIsFib ? pIsFib() : 1;
    CHECK(main_fib != nullptr && child_fib != nullptr && is_fib_while == 1 &&
              fctx.steps == 11 && fctx.magic == 0xF1BE2026u &&
              conv_back == 1 && is_fib_after == 0,
          "Fibers ConvertThreadToFiber/CreateFiber/SwitchToFiber/DeleteFiber roundtrip");

    // 6B. Vista Threadpool (Work, Timer, Wait, Pool + CleanupGroup)
    using CreatePool_t = void*(TRT_STDCALL*)(void*);
    using ClosePool_t = void(TRT_STDCALL*)(void*);
    using CreateCG_t = void*(TRT_STDCALL*)(void);
    using CloseCGM_t = void(TRT_STDCALL*)(void*, int, void*);
    using CloseCG_t = void(TRT_STDCALL*)(void*);
    using InitEnv_t = void(TRT_STDCALL*)(void*);
    using DestroyEnv_t = void(TRT_STDCALL*)(void*);
    using SetEnvPool_t = void(TRT_STDCALL*)(void*, void*);
    using SetEnvCG_t = void(TRT_STDCALL*)(void*, void*, void*);
    using CreateWork_t = void*(TRT_STDCALL*)(void*, void*, void*);
    using SubmitWork_t = void(TRT_STDCALL*)(void*);
    using WaitWork_t = void(TRT_STDCALL*)(void*, int);
    using CloseWork_t = void(TRT_STDCALL*)(void*);
    using CreateTimer_t = void*(TRT_STDCALL*)(void*, void*, void*);
    using SetTimer_t = void(TRT_STDCALL*)(void*, const i64*, u32, u32);
    using IsTimerSet_t = int(TRT_STDCALL*)(void*);
    using WaitTimer_t = void(TRT_STDCALL*)(void*, int);
    using CloseTimer_t = void(TRT_STDCALL*)(void*);
    using CreateWait_t = void*(TRT_STDCALL*)(void*, void*, void*);
    using SetWait_t = void(TRT_STDCALL*)(void*, u64, const i64*);
    using WaitWait_t = void(TRT_STDCALL*)(void*, int);
    using CloseWait_t = void(TRT_STDCALL*)(void*);
    using CreateEv_t = u64(TRT_STDCALL*)(void*, int, int, const u16*);
    using SetEv_t = int(TRT_STDCALL*)(u64);
    using CloseH_t = int(TRT_STDCALL*)(u64);

    auto pCreatePool = (CreatePool_t)kfn("kernel32.dll", "CreateThreadpool");
    auto pClosePool = (ClosePool_t)kfn("kernel32.dll", "CloseThreadpool");
    auto pCreateCG = (CreateCG_t)kfn("kernel32.dll", "CreateThreadpoolCleanupGroup");
    auto pCloseCGM = (CloseCGM_t)kfn("kernel32.dll", "CloseThreadpoolCleanupGroupMembers");
    auto pCloseCG = (CloseCG_t)kfn("kernel32.dll", "CloseThreadpoolCleanupGroup");
    auto pInitEnv = (InitEnv_t)kfn("kernel32.dll", "InitializeThreadpoolEnvironment");
    auto pDestroyEnv = (DestroyEnv_t)kfn("kernel32.dll", "DestroyThreadpoolEnvironment");
    auto pSetEnvPool = (SetEnvPool_t)kfn("kernel32.dll", "SetThreadpoolCallbackPool");
    auto pSetEnvCG = (SetEnvCG_t)kfn("kernel32.dll", "SetThreadpoolCallbackCleanupGroup");
    auto pCreateWork = (CreateWork_t)kfn("kernel32.dll", "CreateThreadpoolWork");
    auto pSubmitWork = (SubmitWork_t)kfn("kernel32.dll", "SubmitThreadpoolWork");
    auto pWaitWork = (WaitWork_t)kfn("kernel32.dll", "WaitForThreadpoolWorkCallbacks");
    auto pCloseWork = (CloseWork_t)kfn("kernel32.dll", "CloseThreadpoolWork");
    auto pCreateTimer = (CreateTimer_t)kfn("kernel32.dll", "CreateThreadpoolTimer");
    auto pSetTimer = (SetTimer_t)kfn("kernel32.dll", "SetThreadpoolTimer");
    auto pIsTimerSet = (IsTimerSet_t)kfn("kernel32.dll", "IsThreadpoolTimerSet");
    auto pWaitTimer = (WaitTimer_t)kfn("kernel32.dll", "WaitForThreadpoolTimerCallbacks");
    auto pCloseTimer = (CloseTimer_t)kfn("kernel32.dll", "CloseThreadpoolTimer");
    auto pCreateWait = (CreateWait_t)kfn("kernel32.dll", "CreateThreadpoolWait");
    auto pSetWait = (SetWait_t)kfn("kernel32.dll", "SetThreadpoolWait");
    auto pWaitWait = (WaitWait_t)kfn("kernel32.dll", "WaitForThreadpoolWaitCallbacks");
    auto pCloseWait = (CloseWait_t)kfn("kernel32.dll", "CloseThreadpoolWait");
    auto pCreateEv = (CreateEv_t)kfn("kernel32.dll", "CreateEventW");
    auto pSetEv = (SetEv_t)kfn("kernel32.dll", "SetEvent");
    auto pCloseH = (CloseH_t)kfn("kernel32.dll", "CloseHandle");

    std::atomic<int> work_hits{0}, timer_hits{0}, wait_hits{0};
    auto tp_work_cb = +[](void*, void* ctx, void*) TRT_STDCALL -> void {
      ((std::atomic<int>*)ctx)->fetch_add(1, std::memory_order_relaxed);
    };
    auto tp_timer_cb = +[](void*, void* ctx, void*) TRT_STDCALL -> void {
      ((std::atomic<int>*)ctx)->fetch_add(1, std::memory_order_relaxed);
    };
    auto tp_wait_cb = +[](void*, void* ctx, void*, u32 wr) TRT_STDCALL -> void {
      if (wr == 0) ((std::atomic<int>*)ctx)->fetch_add(1, std::memory_order_relaxed);
    };

    void* pool = pCreatePool ? pCreatePool(nullptr) : nullptr;
    void* cg = pCreateCG ? pCreateCG() : nullptr;
    u8 env[128] = {};
    if (pInitEnv) pInitEnv(env);
    if (pSetEnvPool) pSetEnvPool(env, pool);
    if (pSetEnvCG) pSetEnvCG(env, cg, nullptr);

    void* wk = pCreateWork ? pCreateWork((void*)tp_work_cb, &work_hits, env) : nullptr;
    if (wk && pSubmitWork && pWaitWork) {
      pSubmitWork(wk);
      pSubmitWork(wk);
      pWaitWork(wk, 0);
    }

    void* tm = pCreateTimer ? pCreateTimer((void*)tp_timer_cb, &timer_hits, env) : nullptr;
    const i64 due10ms = -100000LL;
    int tset_before = 0;
    if (tm && pSetTimer && pIsTimerSet && pWaitTimer) {
      pSetTimer(tm, &due10ms, 0, 0);
      tset_before = pIsTimerSet(tm);
      for (int k = 0; k < 200 && timer_hits.load() == 0; ++k) usleep(2000);
      pWaitTimer(tm, 0);
    }

    u64 ev = pCreateEv ? pCreateEv(nullptr, 1, 0, nullptr) : 0;
    void* wt = (ev && pCreateWait)
                   ? pCreateWait((void*)tp_wait_cb, &wait_hits, env)
                   : nullptr;
    if (wt && pSetWait && pSetEv && pWaitWait) {
      pSetWait(wt, ev, nullptr);
      pSetEv(ev);
      for (int k = 0; k < 200 && wait_hits.load() == 0; ++k) usleep(2000);
      pWaitWait(wt, 0);
    }

    if (pCloseCGM) pCloseCGM(cg, 0, nullptr);
    if (wk && pCloseWork) pCloseWork(wk);
    if (tm && pCloseTimer) pCloseTimer(tm);
    if (wt && pCloseWait) pCloseWait(wt);
    if (ev && pCloseH) pCloseH(ev);
    if (pCloseCG) pCloseCG(cg);
    if (pDestroyEnv) pDestroyEnv(env);
    if (pClosePool) pClosePool(pool);
    CHECK(pool != nullptr && work_hits.load() == 2 && tset_before == 1 &&
              timer_hits.load() >= 1 && wait_hits.load() >= 1,
          "Vista Threadpool Work/Timer/Wait/CleanupGroup execute callbacks and drain cleanly");

    // 6C. CPU topology & kernelbase/kernel32 gaps
    using IsWow2_t = int(TRT_STDCALL*)(void*, u16*, u16*);
    using ActCpu_t = u32(TRT_STDCALL*)(u16);
    using CpuSets_t = int(TRT_STDCALL*)(void*, u32, u32*, void*, u32);
    using Unbiased_t = int(TRT_STDCALL*)(u64*);
    auto pWow2 = (IsWow2_t)kfn("kernel32.dll", "IsWow64Process2");
    auto pActCpu = (ActCpu_t)kfn("kernel32.dll", "GetActiveProcessorCount");
    auto pCpuSets = (CpuSets_t)kfn("kernel32.dll", "GetSystemCpuSetInformation");
    auto pUnbiased = (Unbiased_t)kfn("kernel32.dll", "QueryUnbiasedInterruptTime");
    u16 pm = 0xFFFFu, nm = 0;
    u32 cs_len = 0;
    u8 cs_buf[512] = {};
    u64 qut = 0;
    CHECK(pWow2 && pWow2((void*)(uintptr_t)-1, &pm, &nm) == 1 &&
              pm == 0 && nm == 0x8664u &&
              pActCpu && pActCpu(0) >= 1 &&
              pCpuSets && pCpuSets(cs_buf, sizeof(cs_buf), &cs_len, nullptr, 0) == 1 &&
              cs_len > 0 && pUnbiased && pUnbiased(&qut) == 1 && qut > 0,
          "IsWow64Process2, GetActiveProcessorCount, GetSystemCpuSetInformation, QueryUnbiasedInterruptTime succeed");

    // 6D. powrprof / pdh / wer / propsys / userenv / wtsapi32
    using PwrCreate_t = void*(TRT_GUEST_ABI*)(void*);
    using PwrSet_t = int(TRT_GUEST_ABI*)(void*, u32);
    using PwrClose_t = int(TRT_GUEST_ABI*)(void*);
    using PwrRole_t = u32(TRT_GUEST_ABI*)(void);
    auto pPwrCreate = (PwrCreate_t)kfn("powrprof.dll", "PowerCreateRequest");
    auto pPwrSet = (PwrSet_t)kfn("powrprof.dll", "PowerSetRequest");
    auto pPwrClear = (PwrSet_t)kfn("powrprof.dll", "PowerClearRequest");
    auto pPwrClose = (PwrClose_t)kfn("powrprof.dll", "PowerCloseRequest");
    auto pPwrRole = (PwrRole_t)kfn("powrprof.dll", "PowerDeterminePlatformRole");
    void* preq = pPwrCreate ? pPwrCreate(nullptr) : nullptr;
    CHECK(preq && pPwrSet && pPwrSet(preq, 0) == 1 &&
              pPwrClear && pPwrClear(preq, 0) == 1 &&
              pPwrClose && pPwrClose(preq) == 1 &&
              pPwrRole && pPwrRole() >= 1,
          "powrprof.dll PowerCreateRequest/SetRequest/ClearRequest/PlatformRole succeed");

    using PdhOpen_t = i32(TRT_GUEST_ABI*)(const u16*, u64, void**);
    using PdhAdd_t = i32(TRT_GUEST_ABI*)(void*, const u16*, u64, void**);
    using PdhCol_t = i32(TRT_GUEST_ABI*)(void*);
    using PdhFmt_t = i32(TRT_GUEST_ABI*)(void*, u32, u32*, void*);
    auto pPdhOpen = (PdhOpen_t)kfn("pdh.dll", "PdhOpenQueryW");
    auto pPdhAdd = (PdhAdd_t)kfn("pdh.dll", "PdhAddEnglishCounterW");
    auto pPdhCol = (PdhCol_t)kfn("pdh.dll", "PdhCollectQueryData");
    auto pPdhFmt = (PdhFmt_t)kfn("pdh.dll", "PdhGetFormattedCounterValue");
    auto pPdhClose = (PdhCol_t)kfn("pdh.dll", "PdhCloseQuery");
    void* pdhq = nullptr;
    void* pdhc = nullptr;
    u8 pdh_val[16] = {};
    double pdh_dbl = 0.0;
    const u16 k_mem_ctr[] = {'\\', 'M', 'e', 'm', 'o', 'r', 'y', '\\', 'A', 'v',
                             'a', 'i', 'l', 'a', 'b', 'l', 'e', ' ', 'M', 'B',
                             'y', 't', 'e', 's', 0};
    const bool pdh_ok = pPdhOpen && pPdhOpen(nullptr, 0, &pdhq) == 0 &&
                        pPdhAdd && pPdhAdd(pdhq, k_mem_ctr, 0, &pdhc) == 0 &&
                        pPdhCol && pPdhCol(pdhq) == 0 &&
                        pPdhFmt && pPdhFmt(pdhc, 0x200u, nullptr, pdh_val) == 0;
    if (pdh_ok) memcpy(&pdh_dbl, pdh_val + 8, sizeof(double));
    if (pdhq && pPdhClose) pPdhClose(pdhq);
    CHECK(pdh_ok && pdh_dbl > 100.0,
          "pdh.dll OpenQuery/AddEnglishCounter/CollectQueryData/GetFormattedCounterValue works");

    using WerSet_t = i32(TRT_GUEST_ABI*)(u32);
    using WerGet_t = i32(TRT_GUEST_ABI*)(void*, u32*);
    using PsCreate_t = i32(TRT_GUEST_ABI*)(const void*, void**);
    using PsGetSet_t = i32(TRT_GUEST_ABI*)(void*, const void*, void*);
    using UenvDir_t = int(TRT_GUEST_ABI*)(void*, u16*, u32*);
    using WtsQ_t = int(TRT_GUEST_ABI*)(void*, u32, u32, void**, u32*);
    using WtsFree_t = void(TRT_GUEST_ABI*)(void*);
    auto pWerSet = (WerSet_t)kfn("wer.dll", "WerSetFlags");
    auto pWerGet = (WerGet_t)kfn("wer.dll", "WerGetFlags");
    auto pPsCreate = (PsCreate_t)kfn("propsys.dll", "PSCreateMemoryPropertyStore");
    auto pUenvDir = (UenvDir_t)kfn("userenv.dll", "GetUserProfileDirectoryW");
    auto pWtsQ = (WtsQ_t)kfn("wtsapi32.dll", "WTSQuerySessionInformationW");
    auto pWtsFree = (WtsFree_t)kfn("wtsapi32.dll", "WTSFreeMemory");
    u32 wflags = 0;
    void* pstore = nullptr;
    u8 pk[20] = {1, 2, 3, 4}, pv_in[24] = {}, pv_out[24] = {};
    *(u16*)pv_in = 19u;
    *(u32*)(pv_in + 8) = 0xCAFE1234u;
    if (pPsCreate && pPsCreate(nullptr, &pstore) == 0 && pstore) {
      auto** vt = *(void***)pstore;
      ((i32(TRT_GUEST_ABI*)(void*, const void*, const void*))vt[6])(pstore, pk, pv_in);
      ((i32(TRT_GUEST_ABI*)(void*, const void*, void*))vt[5])(pstore, pk, pv_out);
      ((u32(TRT_GUEST_ABI*)(void*))vt[2])(pstore);
    }
    u16 uprof[128] = {};
    u32 ulen = 128;
    void* wts_buf = nullptr;
    u32 wts_bytes = 0;
    const bool wts_ok = pWtsQ && pWtsQ(nullptr, 1, 4 /*WTSSessionId*/, &wts_buf, &wts_bytes) == 1 &&
                        wts_buf && wts_bytes == 4 && *(u32*)wts_buf == 1u;
    if (wts_buf && pWtsFree) pWtsFree(wts_buf);
    CHECK(pWerSet && pWerSet(7) == 0 && pWerGet && pWerGet((void*)-1, &wflags) == 0 &&
              wflags == 7 && *(u32*)(pv_out + 8) == 0xCAFE1234u &&
              pUenvDir && pUenvDir((void*)-4, uprof, &ulen) == 1 && uprof[0] == 'C' && wts_ok,
          "wer.dll, propsys.dll, userenv.dll, and wtsapi32.dll roundtrip cleanly");

    // 6E. Async ReadFileEx/WriteFileEx APCs, QueueUserAPC, WaitForSingleObjectEx,
    //     FLS destructors on thread exit, and dbghelp line/sym lookup exports
    using WriteEx_t = int(TRT_STDCALL*)(void*, const void*, u32, void*, void*);
    using ReadEx_t = int(TRT_STDCALL*)(void*, void*, u32, void*, void*);
    using SleepEx_t = u32(TRT_STDCALL*)(u32, int);
    using QueueApc_t = u32(TRT_STDCALL*)(void*, u64, u64);
    using WaitEx_t = u32(TRT_STDCALL*)(u64, u32, int);
    using FlsAlloc_t = u32(TRT_STDCALL*)(void*);
    using FlsSet_t = int(TRT_STDCALL*)(u32, void*);
    using FlsFree_t = int(TRT_STDCALL*)(u32);
    using SymInit_t = int(TRT_STDCALL*)(void*, const char*, int);
    using SymLine64_t = int(TRT_STDCALL*)(void*, u64, u32*, void*);
    using SymClean_t = int(TRT_STDCALL*)(void*);

    auto pWriteEx = (WriteEx_t)kfn("kernel32.dll", "WriteFileEx");
    auto pReadEx = (ReadEx_t)kfn("kernel32.dll", "ReadFileEx");
    auto pSleepEx = (SleepEx_t)kfn("kernel32.dll", "SleepEx");
    auto pQueueApc = (QueueApc_t)kfn("kernel32.dll", "QueueUserAPC");
    auto pWaitEx = (WaitEx_t)kfn("kernel32.dll", "WaitForSingleObjectEx");
    auto pFlsAlloc = (FlsAlloc_t)kfn("kernel32.dll", "FlsAlloc");
    auto pFlsSet = (FlsSet_t)kfn("kernel32.dll", "FlsSetValue");
    auto pFlsFree = (FlsFree_t)kfn("kernel32.dll", "FlsFree");
    auto pSymInit = (SymInit_t)kfn("dbghelp.dll", "SymInitialize");
    auto pSymLine64 = (SymLine64_t)kfn("dbghelp.dll", "SymGetLineFromAddr64");
    auto pSymClean = (SymClean_t)kfn("dbghelp.dll", "SymCleanup");

    struct IoApcRec { u32 err; u32 bytes; void* ov; };
    static IoApcRec g_io_rec{};
    static std::atomic<int> g_io_hits{0};
    auto io_apc_cb = +[](u32 err, u32 bytes, void* ov) TRT_STDCALL -> void {
      g_io_rec = {err, bytes, ov};
      g_io_hits.fetch_add(1, std::memory_order_relaxed);
    };
    static std::atomic<u64> g_user_apc_val{0};
    auto user_apc_cb = +[](u64 data) TRT_STDCALL -> void {
      g_user_apc_val.store(data, std::memory_order_release);
    };
    static std::atomic<u64> g_fls_exit_val{0};
    auto fls_cb = +[](void* val) TRT_STDCALL -> void {
      g_fls_exit_val.store((u64)(uintptr_t)val, std::memory_order_release);
    };

    u16 apc_path[64];
    make_u16("c:\\apc_wave6.bin", apc_path, 64);
    void* af = trt_CreateFileW(apc_path, 0xC0000000u, 3, nullptr, 2, 0x40000080u, nullptr);
    u64 wov[4] = {0, 0, 0, 0x123456789ABCDEF0ull};
    u64 rov[4] = {0, 0, 0, 0xFEDCBA9876543210ull};
    char rapc[16] = {};
    const int wr_ok = (af && pWriteEx) ? pWriteEx(af, "APC_STREAM!", 11, wov, (void*)io_apc_cb) : 0;
    const u32 sl_wr = pSleepEx ? pSleepEx(0, 1) : 0;
    g_io_rec = {};
    const int rd_ok = (af && pReadEx) ? pReadEx(af, rapc, 11, rov, (void*)io_apc_cb) : 0;
    const u32 sl_rd = pSleepEx ? pSleepEx(0, 1) : 0;
    if (af) trt_CloseHandle(af);

    const u64 ev_apc = trt_CreateEventW(nullptr, true, false, nullptr);
    const u32 q_ok = pQueueApc ? pQueueApc((void*)user_apc_cb, (u64)(i64)-2, 0xDEAD5150ull) : 0;
    const u32 w_apc = (ev_apc && pWaitEx) ? pWaitEx(ev_apc, 50, 1) : 0;
    if (ev_apc) trt_CloseHandle((void*)(uintptr_t)ev_apc);

    struct FlsWorkerArgs { FlsSet_t set; u32 idx; };
    const u32 fls_idx = pFlsAlloc ? pFlsAlloc((void*)fls_cb) : 0xFFFFFFFFu;
    FlsWorkerArgs fargs{pFlsSet, fls_idx};
    auto fls_worker = +[](void* raw) TRT_STDCALL -> u32 {
      FlsWorkerArgs* a = (FlsWorkerArgs*)raw;
      if (a->set && a->idx != 0xFFFFFFFFu) a->set(a->idx, (void*)(uintptr_t)0xF15B00B5ull);
      return 0;
    };
    const u64 fth = trt_CreateThread(nullptr, nullptr, (void*)fls_worker, &fargs, 0, nullptr);
    if (fth) {
      trt_WaitForSingleObject(fth, 1000);
      trt_CloseHandle((void*)(uintptr_t)fth);
    }
    if (fls_idx != 0xFFFFFFFFu && pFlsFree) pFlsFree(fls_idx);

    u8 line64[40] = {};
    *(u32*)line64 = sizeof(line64);
    u32 line_disp = 0xFFFFFFFFu;
    const int sym_init_ok = pSymInit ? pSymInit((void*)(uintptr_t)-1, nullptr, 0) : 0;
    if (pSymLine64) pSymLine64((void*)(uintptr_t)-1, (u64)(uintptr_t)&trt_ReadFile, &line_disp, line64);
    const int sym_clean_ok = pSymClean ? pSymClean((void*)(uintptr_t)-1) : 0;

    CHECK(wr_ok == 1 && sl_wr == 0xC0u && rd_ok == 1 && sl_rd == 0xC0u &&
              g_io_hits.load() == 2 && g_io_rec.err == 0 && g_io_rec.bytes == 11 &&
              g_io_rec.ov == rov && wov[3] == 0x123456789ABCDEF0ull &&
              rov[3] == 0xFEDCBA9876543210ull && memcmp(rapc, "APC_STREAM!", 11) == 0,
          "ReadFileEx/WriteFileEx complete via SleepEx(..., TRUE) without touching hEvent");
    CHECK(q_ok == 1 && w_apc == 0xC0u && g_user_apc_val.load() == 0xDEAD5150ull,
          "QueueUserAPC wakes WaitForSingleObjectEx(..., TRUE) with WAIT_IO_COMPLETION");
    CHECK(g_fls_exit_val.load() == 0xF15B00B5ull,
          "FLS callback runs on exiting worker thread before handle signal");
    CHECK(sym_init_ok == 1 && sym_clean_ok == 1 && line_disp == 0,
          "dbghelp.dll SymInitialize/SymGetLineFromAddr64/SymCleanup succeed");
  }

  // ---- Wave 6F: Universal PC/Steam engine runtime, API-set, CRT, and drivers ----
  {
    auto kfn = [&](const char* mod, const char* sym) -> void* {
      return trt_resolve_module(nullptr, mod, sym, 0);
    };
    auto eq_u16_ascii = [](const u16* w, const char* a) -> bool {
      if (!w || !a) return false;
      size_t i = 0;
      for (; a[i]; ++i) {
        if (w[i] != (u16)(unsigned char)a[i]) return false;
      }
      return w[i] == 0;
    };

    // 1. API-set routing fixes (registry/version/path/legacy/ext-ms)
    const char* t_wreg = trt_apiset_target("api-ms-win-core-winrt-registry-l1-1-0.dll");
    const char* t_reg  = trt_apiset_target("api-ms-win-core-registry-l1-1-0.dll");
    const char* t_ver  = trt_apiset_target("api-ms-win-core-version-l1-1-0.dll");
    const char* t_path = trt_apiset_target("api-ms-win-core-path-l1-1-0.dll");
    const char* t_leg  = trt_apiset_target("api-ms-win-core-kernel32-legacy-l1-1-0.dll");
    const char* t_ext  = trt_apiset_target("ext-ms-win-ntuser-window-l1-1-0.dll");
    CHECK(t_wreg && strcmp(t_wreg, "advapi32.dll") == 0 &&
              t_reg && strcmp(t_reg, "advapi32.dll") == 0 &&
              t_ver && strcmp(t_ver, "version.dll") == 0 &&
              t_path && strcmp(t_path, "kernelbase.dll") == 0 &&
              t_leg && strcmp(t_leg, "kernel32.dll") == 0 &&
              t_ext && strcmp(t_ext, "user32.dll") == 0 &&
              trt_apiset_is_contract("ext-ms-win-ntuser-window-l1-1-0.dll") == 1,
          "Wave 6F: apiset routes registry/version/path/kernel32-legacy/ext-ms correctly");

    // 2. PathCchSkipRoot / PathCchCombineEx / PathCchCanonicalizeEx
    using PathSkipRoot_t = i32(TRT_STDCALL*)(const u16*, const u16**);
    using PathCombEx_t   = i32(TRT_STDCALL*)(u16*, u64, const u16*, const u16*, u32);
    using PathCanonEx_t  = i32(TRT_STDCALL*)(u16*, u64, const u16*, u32);
    auto pSkipRoot = (PathSkipRoot_t)kfn("kernelbase.dll", "PathCchSkipRoot");
    auto pCombEx   = (PathCombEx_t)kfn("kernelbase.dll", "PathCchCombineEx");
    auto pCanonEx  = (PathCanonEx_t)kfn("kernelbase.dll", "PathCchCanonicalizeEx");
    u16 w_in1[64], w_in2[64], w_out[128], w_canon_in[64], w_canon_out[128];
    make_u16("C:\\games\\foo\\", w_in1, 64);
    make_u16("..\\bar\\.\\baz.exe", w_in2, 64);
    make_u16("C:\\x\\..\\y\\.\\z", w_canon_in, 64);
    const u16* root_end = nullptr;
    const i32 sr_hr = pSkipRoot ? pSkipRoot(w_in1, &root_end) : -1;
    const i32 cb_hr = pCombEx ? pCombEx(w_out, 128, w_in1, w_in2, 0) : -1;
    const i32 cn_hr = pCanonEx ? pCanonEx(w_canon_out, 128, w_canon_in, 0) : -1;
    CHECK(sr_hr == 0 && root_end == w_in1 + 3 &&
              cb_hr == 0 && eq_u16_ascii(w_out, "C:\\games\\bar\\baz.exe") &&
              cn_hr == 0 && eq_u16_ascii(w_canon_out, "C:\\y\\z"),
          "Wave 6F: PathCchSkipRoot/PathCchCombineEx/PathCchCanonicalizeEx normalize paths");

    // 3. ntdll VM, device family, ANSI/UNICODE strings
    using NtAllocEx_t = u32(TRT_GUEST_ABI*)(void*, void**, u64*, u32, u32, void*, u32);
    using NtFreeVm_t  = u32(TRT_GUEST_ABI*)(void*, void**, u64*, u32);
    using DevFam_t    = void(TRT_GUEST_ABI*)(u64*, u32*, u32*);
    using InitAnsi_t  = void(TRT_GUEST_ABI*)(void*, const char*);
    using A2U_t       = u32(TRT_GUEST_ABI*)(void*, const void*, u8);
    using FreeUni_t   = void(TRT_GUEST_ABI*)(void*);
    auto pNtAllocEx = (NtAllocEx_t)kfn("ntdll.dll", "NtAllocateVirtualMemoryEx");
    auto pNtFreeVm  = (NtFreeVm_t)kfn("ntdll.dll", "NtFreeVirtualMemory");
    auto pDevFam    = (DevFam_t)kfn("ntdll.dll", "RtlGetDeviceFamilyInfoEnum");
    auto pInitAnsi  = (InitAnsi_t)kfn("ntdll.dll", "RtlInitAnsiString");
    auto pA2U       = (A2U_t)kfn("ntdll.dll", "RtlAnsiStringToUnicodeString");
    auto pFreeUni   = (FreeUni_t)kfn("ntdll.dll", "RtlFreeUnicodeString");
    void* va = nullptr;
    u64 vsz = 4096;
    const u32 st_alloc = pNtAllocEx ? pNtAllocEx((void*)(uintptr_t)-1, &va, &vsz, 0x3000u, 0x04u, nullptr, 0) : 1u;
    if (st_alloc == 0 && va) memset(va, 0x5Au, 64);
    u64 fsz = 0;
    const u32 st_free = (st_alloc == 0 && va && pNtFreeVm) ? pNtFreeVm((void*)(uintptr_t)-1, &va, &fsz, 0x8000u) : 1u;
    u64 uap = 0;
    u32 fam = 0, form = 99;
    if (pDevFam) pDevFam(&uap, &fam, &form);
    struct NtAnsiStr { u16 len; u16 max_len; u32 pad; char* buf; };
    struct NtUniStr  { u16 len; u16 max_len; u32 pad; u16* buf; };
    NtAnsiStr astr{};
    NtUniStr ustr{};
    if (pInitAnsi) pInitAnsi(&astr, "Tiramisu");
    const u32 a2u_st = (pInitAnsi && pA2U) ? pA2U(&ustr, &astr, 1) : 1u;
    const bool a2u_ok = (a2u_st == 0 && ustr.len == 16 && ustr.buf && eq_u16_ascii(ustr.buf, "Tiramisu"));
    if (pFreeUni) pFreeUni(&ustr);
    CHECK(st_alloc == 0 && st_free == 0 && fam == 3u && a2u_ok && ustr.buf == nullptr,
          "Wave 6F: ntdll NtAllocateVirtualMemoryEx, DeviceFamily=DESKTOP, ANSI->Unicode roundtrip");

    // 4. Dynamic JIT .pdata unwind registration
    using RtlAddFn_t = u8(TRT_STDCALL*)(void*, u32, u64);
    using RtlDelFn_t = u8(TRT_STDCALL*)(void*);
    using RtlLook_t  = void*(TRT_STDCALL*)(u64, u64*, void*);
    using RtlPcHdr_t = void*(TRT_STDCALL*)(u64, u64*);
    auto pAddFn = (RtlAddFn_t)kfn("kernel32.dll", "RtlAddFunctionTable");
    auto pDelFn = (RtlDelFn_t)kfn("kernel32.dll", "RtlDeleteFunctionTable");
    auto pLook  = (RtlLook_t)kfn("ntdll.dll", "RtlLookupFunctionEntry");
    auto pPcHdr = (RtlPcHdr_t)kfn("ntdll.dll", "RtlPcToFileHeader");
    struct JitRuntimeFn { u32 begin; u32 end; u32 unwind; } jit_tbl[1] = {
        {0x100u, 0x200u, 0x300u}};
    const u64 jit_base = 0x0000123400000000ULL;
    const u8 add_ok = pAddFn ? pAddFn(jit_tbl, 1, jit_base) : 0;
    u64 look_base = 0, hdr_base = 0;
    void* got_fn = pLook ? pLook(jit_base + 0x150u, &look_base, nullptr) : nullptr;
    void* got_hdr = pPcHdr ? pPcHdr(jit_base + 0x150u, &hdr_base) : nullptr;
    const u8 del_ok = pDelFn ? pDelFn(jit_tbl) : 0;
    u64 after_base = 0;
    void* after_fn = pLook ? pLook(jit_base + 0x150u, &after_base, nullptr) : (void*)1;
    CHECK(add_ok == 1 && got_fn == (void*)&jit_tbl[0] && look_base == jit_base &&
              got_hdr == (void*)(uintptr_t)jit_base && hdr_base == jit_base &&
              del_ok == 1 && after_fn == nullptr,
          "Wave 6F: RtlAddFunctionTable/RtlLookupFunctionEntry/RtlPcToFileHeader/RtlDeleteFunctionTable");

    // 5. Kernel32 gap: LockFileEx/UnlockFileEx, GetDiskFreeSpaceW, Drives, Tz, NUMA, IoRing
    using LockEx_t   = int(TRT_STDCALL*)(void*, u32, u32, u32, u32, void*);
    using UnlockEx_t = int(TRT_STDCALL*)(void*, u32, u32, u32, void*);
    using DiskFree_t = int(TRT_STDCALL*)(const u16*, u32*, u32*, u32*, u32*);
    using DriveStr_t = u32(TRT_STDCALL*)(u32, u16*);
    using Tz2Utc_t   = int(TRT_STDCALL*)(const void*, const u16*, u16*);
    using NumaHi_t   = int(TRT_STDCALL*)(u32*);
    using NumaMask_t = int(TRT_STDCALL*)(u16, void*);
    using IoCap_t    = i32(TRT_STDCALL*)(void*);
    using IoCreate_t = i32(TRT_STDCALL*)(u32, u32, u32, u32, void**);
    auto pLockEx   = (LockEx_t)kfn("kernel32.dll", "LockFileEx");
    auto pUnlockEx = (UnlockEx_t)kfn("kernel32.dll", "UnlockFileEx");
    auto pDiskFree = (DiskFree_t)kfn("kernel32.dll", "GetDiskFreeSpaceW");
    auto pDriveStr = (DriveStr_t)kfn("kernel32.dll", "GetLogicalDriveStringsW");
    auto pTz2Utc   = (Tz2Utc_t)kfn("kernel32.dll", "TzSpecificLocalTimeToSystemTime");
    auto pNumaHi   = (NumaHi_t)kfn("kernel32.dll", "GetNumaHighestNodeNumber");
    auto pNumaMask = (NumaMask_t)kfn("kernel32.dll", "GetNumaNodeProcessorMaskEx");
    auto pIoCap    = (IoCap_t)kfn("kernelbase.dll", "QueryIoRingCapabilities");
    auto pIoCreate = (IoCreate_t)kfn("kernelbase.dll", "CreateIoRing");

    u16 lock_path[64];
    make_u16("C:\\w6f_lock.bin", lock_path, 64);
    void* lf = trt_CreateFileW(lock_path, 0xC0000000u, 3, nullptr, 2, 0x80u, nullptr);
    u32 lov[8] = {};
    const int lk1 = (lf && pLockEx) ? pLockEx(lf, 0x3u, 0, 64, 0, lov) : 0;
    const int lk2 = (lf && pLockEx) ? pLockEx(lf, 0x3u, 0, 32, 0, lov) : 1;
    const u32 lk2_err = trt_GetLastError();
    const int ulk = (lf && pUnlockEx) ? pUnlockEx(lf, 0, 64, 0, lov) : 0;
    const int lk3 = (lf && pLockEx) ? pLockEx(lf, 0x3u, 0, 64, 0, lov) : 0;
    if (lf && pUnlockEx) pUnlockEx(lf, 0, 64, 0, lov);
    if (lf) trt_CloseHandle(lf);

    u32 spc = 0, bps = 0, fc = 0, tc = 0;
    const int df_ok = pDiskFree ? pDiskFree(nullptr, &spc, &bps, &fc, &tc) : 0;
    u16 drvs[64] = {};
    const u32 dlen = pDriveStr ? pDriveStr(64, drvs) : 0;
    u16 sys_in[8] = {2026, 11, 6, 14, 12, 30, 0, 0};
    u16 sys_out[8] = {};
    const int tz_ok = pTz2Utc ? pTz2Utc(nullptr, sys_in, sys_out) : 0;
    u32 numa_hi = 99;
    struct { u64 mask; u16 grp; u16 r[3]; } gaff{};
    const int nh_ok = pNumaHi ? pNumaHi(&numa_hi) : 0;
    const int nm_ok = pNumaMask ? pNumaMask(0, &gaff) : 0;
    u32 io_caps[3] = {99u, 99u, 99u};
    void* io_h = (void*)1;
    const i32 cap_hr = pIoCap ? pIoCap(io_caps) : -1;
    const i32 cr_hr  = pIoCreate ? pIoCreate(1, 0, 8, 8, &io_h) : 0;
    CHECK(lk1 == 1 && lk2 == 0 && lk2_err == 33u && ulk == 1 && lk3 == 1 &&
              df_ok == 1 && spc > 0 && bps > 0 && tc >= fc &&
              dlen >= 9 && eq_u16_ascii(drvs, "C:\\") &&
              tz_ok == 1 && memcmp(sys_in, sys_out, sizeof(sys_in)) == 0 &&
              nh_ok == 1 && numa_hi == 0 && nm_ok == 1 && gaff.mask != 0 &&
              cap_hr == 0 && io_caps[0] == 0 && cr_hr == (i32)0x80004001u && io_h == nullptr,
          "Wave 6F: LockFileEx, DiskFreeSpace, DriveStrings, Tz, NUMA, and honest IoRing");

    // 6. Registry (RegDeleteKeyExW/RegDeleteTreeW/RegFlushKey) & LsaNtStatusToWinError
    using RegCreate_t  = int(TRT_GUEST_ABI*)(u64, const u16*, u32, void*, u32, u32, void**, u32*);
    using RegOpen_t    = int(TRT_GUEST_ABI*)(u64, const u16*, u32, u32, void**);
    using RegClose_t   = int(TRT_GUEST_ABI*)(u64);
    using RegFlush_t   = int(TRT_GUEST_ABI*)(u64);
    using RegDelEx_t   = int(TRT_GUEST_ABI*)(u64, const u16*, u32, u32);
    using RegDelTree_t = int(TRT_GUEST_ABI*)(u64, const u16*);
    using LsaErr_t     = u32(TRT_GUEST_ABI*)(u32);
    auto pRegCreate  = (RegCreate_t)kfn("advapi32.dll", "RegCreateKeyExW");
    auto pRegOpen    = (RegOpen_t)kfn("advapi32.dll", "RegOpenKeyExW");
    auto pRegClose   = (RegClose_t)kfn("advapi32.dll", "RegCloseKey");
    auto pRegFlush   = (RegFlush_t)kfn("advapi32.dll", "RegFlushKey");
    auto pRegDelEx   = (RegDelEx_t)kfn("advapi32.dll", "RegDeleteKeyExW");
    auto pRegDelTree = (RegDelTree_t)kfn("advapi32.dll", "RegDeleteTreeW");
    auto pLsaErr     = (LsaErr_t)kfn("advapi32.dll", "LsaNtStatusToWinError");
    u16 w_reg_sub[64], w_reg_leaf[64], w_reg_root[64];
    make_u16("Software\\TiramisuW6F\\Sub", w_reg_sub, 64);
    make_u16("Software\\TiramisuW6F\\Leaf", w_reg_leaf, 64);
    make_u16("Software\\TiramisuW6F", w_reg_root, 64);
    void *hk1 = nullptr, *hk2 = nullptr, *hk_chk = nullptr;
    const int rc1 = pRegCreate ? pRegCreate(0x80000001ULL, w_reg_sub, 0, nullptr, 0, 0x20006u, &hk1, nullptr) : -1;
    const int rc2 = pRegCreate ? pRegCreate(0x80000001ULL, w_reg_leaf, 0, nullptr, 0, 0x20006u, &hk2, nullptr) : -1;
    if (hk1 && pRegClose) pRegClose((u64)(uintptr_t)hk1);
    if (hk2 && pRegClose) pRegClose((u64)(uintptr_t)hk2);
    const int rfl = pRegFlush ? pRegFlush(0x80000001ULL) : -1;
    const int rdl = pRegDelEx ? pRegDelEx(0x80000001ULL, w_reg_leaf, 0, 0) : -1;
    const int rdt = pRegDelTree ? pRegDelTree(0x80000001ULL, w_reg_root) : -1;
    const int rop = pRegOpen ? pRegOpen(0x80000001ULL, w_reg_root, 0, 0x20019u, &hk_chk) : 0;
    const u32 lsa_acc = pLsaErr ? pLsaErr(0xC0000022u) : 0u;
    CHECK(rc1 == 0 && rc2 == 0 && rfl == 0 && rdl == 0 && rdt == 0 &&
              rop == 2 /*ERROR_FILE_NOT_FOUND*/ && lsa_acc == 5u,
          "Wave 6F: RegFlushKey/RegDeleteKeyExW/RegDeleteTreeW & LsaNtStatusToWinError");

    // 7. User32 gap, SteamAPI_InitFlat, and Desktop DLL BUILTIN bridges
    using NonClientDpi_t = int(TRT_STDCALL*)(void*);
    using TouchHist_t    = int(TRT_STDCALL*)(u32, u32*, void*);
    using SteamFlat_t    = int(TRT_GUEST_ABI*)(char*);
    using VkCreate_t     = i32(TRT_GUEST_ABI*)(const void*, const void*, void**);
    using VkProc_t       = void*(TRT_GUEST_ABI*)(void*, const char*);
    using IsThemed_t     = int(TRT_GUEST_ABI*)(void);
    using WinTrust_t     = i32(TRT_GUEST_ABI*)(void*, void*, void*);
    auto pDpiNc    = (NonClientDpi_t)kfn("user32.dll", "EnableNonClientDpiScaling");
    auto pTouch    = (TouchHist_t)kfn("user32.dll", "GetPointerTouchInfoHistory");
    auto pSFlat    = (SteamFlat_t)kfn("steam_api64.dll", "SteamAPI_InitFlat");
    auto pVkCreate = (VkCreate_t)kfn("vulkan-1.dll", "vkCreateInstance");
    auto pVkProc   = (VkProc_t)kfn("vulkan-1.dll", "vkGetInstanceProcAddr");
    auto pThemed   = (IsThemed_t)kfn("uxtheme.dll", "IsThemeActive");
    auto pTrust    = (WinTrust_t)kfn("wintrust.dll", "WinVerifyTrust");
    u32 touch_cnt = 4;
    u8 touch_buf[32] = {};
    const int dpi_ok = pDpiNc ? pDpiNc((void*)(uintptr_t)1) : 0;
    const int tch_ok = pTouch ? pTouch(1, &touch_cnt, touch_buf) : 1;
    const u32 tch_err = trt_GetLastError();
    char steam_err[128] = "dirty";
    const int sf_rc = pSFlat ? pSFlat(steam_err) : -1;
    void* vk_inst = nullptr;
    struct {
      u32 sType; const void* pNext; u32 flags; const void* pApp;
      u32 lCnt; const char* const* ppL; u32 eCnt; const char* const* ppE;
    } vk_ci = {1u, nullptr, 0u, nullptr, 0u, nullptr, 0u, nullptr};
    const i32 vk_null_rc = pVkCreate ? pVkCreate(nullptr, nullptr, &vk_inst) : 0;
    const i32 vk_ok_rc   = pVkCreate ? pVkCreate(&vk_ci, nullptr, &vk_inst) : -1;
    void* vk_resolved    = pVkProc ? pVkProc(nullptr, "vkCreateInstance") : nullptr;
    using VkDestroy_t = void(TRT_GUEST_ABI*)(void*, const void*);
    auto pVkDestroy = (VkDestroy_t)kfn("vulkan-1.dll", "vkDestroyInstance");
    if (pVkDestroy && vk_inst) pVkDestroy(vk_inst, nullptr);
    CHECK(dpi_ok == 1 && tch_ok == 0 && touch_cnt == 0 && tch_err == 87u &&
              sf_rc == 0 && steam_err[0] == 0 &&
              vk_null_rc == -3 && vk_ok_rc == 0 && vk_inst != nullptr &&
              vk_resolved == (void*)pVkCreate &&
              pThemed && pThemed() == 0 && pTrust && pTrust(nullptr, nullptr, nullptr) == (i32)0x800B0100u,
          "Wave 6F: User32 DPI/Pointer, SteamAPI_InitFlat, Vulkan/uxtheme/wintrust bridges");

    // 8. msvcrt swprintf / sscanf / math / _waccess / _wstat64
    using Swprintf_t = i32(TRT_GUEST_ABI*)(u16*, u64, const u16*, ...);
    using Sscanf_t   = i32(TRT_GUEST_ABI*)(const char*, const char*, ...);
    using Trig_t     = double(TRT_GUEST_ABI*)(double);
    using WAccess_t  = i32(TRT_GUEST_ABI*)(const u16*, i32);
    using WStat64_t  = i32(TRT_GUEST_ABI*)(const u16*, void*);
    auto pSwprintf = (Swprintf_t)kfn("msvcrt.dll", "swprintf");
    auto pSscanf   = (Sscanf_t)kfn("msvcrt.dll", "sscanf");
    auto pSin      = (Trig_t)kfn("msvcrt.dll", "sin");
    auto pCos      = (Trig_t)kfn("msvcrt.dll", "cos");
    auto pWAccess  = (WAccess_t)kfn("msvcrt.dll", "_waccess");
    auto pWStat64  = (WStat64_t)kfn("msvcrt.dll", "_wstat64");
    u16 wfmt[32], warg[16], wres[64];
    make_u16("%s-%d", wfmt, 32);
    make_u16("xbo", warg, 16);
    const i32 sw_n = pSwprintf ? pSwprintf(wres, 64, wfmt, warg, 360) : -1;
    i32 iv = 0;
    double dv = 0.0;
    u32 hv = 0;
    const i32 ss_n = pSscanf ? pSscanf("42 3.5 hex=1f", "%d %lf hex=%x", &iv, &dv, &hv) : 0;
    const double sv = pSin ? pSin(0.0) : 9.0;
    const double cv = pCos ? pCos(0.0) : 9.0;
    u16 stat_path[64];
    make_u16("C:\\w6f_stat.txt", stat_path, 64);
    void* sf = trt_CreateFileW(stat_path, 0xC0000000u, 3, nullptr, 2, 0x80u, nullptr);
    u32 written = 0;
    if (sf) {
      trt_WriteFile(sf, "hello!", 6, &written, nullptr);
      trt_CloseHandle(sf);
    }
    struct W64Stat {
      u32 st_dev; u16 st_ino; u16 st_mode; i16 st_nlink; i16 st_uid; i16 st_gid;
      u16 _pad1; u32 st_rdev; u32 _pad2; i64 st_size; i64 a; i64 m; i64 c;
    } st64{};
    const i32 acc_rc = pWAccess ? pWAccess(stat_path, 0) : -1;
    const i32 st_rc  = pWStat64 ? pWStat64(stat_path, &st64) : -1;
    CHECK(sw_n == 7 && eq_u16_ascii(wres, "xbo-360") &&
              ss_n == 3 && iv == 42 && dv > 3.49 && dv < 3.51 && hv == 0x1fu &&
              (sv < 0.001 && sv > -0.001) && (cv > 0.999 && cv < 1.001) &&
              acc_rc == 0 && st_rc == 0 && st64.st_size == 6,
          "Wave 6F: msvcrt swprintf/sscanf/sin/cos/_waccess/_wstat64 roundtrip");

    // 9. Alertable GetQueuedCompletionStatusEx wakes on QueueUserAPC
    using CreateIocp_t = u64(TRT_STDCALL*)(void*, u64, u64, u32);
    using GqcsEx_t     = int(TRT_STDCALL*)(u64, void*, u32, u32*, u32, int);
    using QueueApc_t   = u32(TRT_STDCALL*)(void*, u64, u64);
    auto pIocp    = (CreateIocp_t)kfn("kernel32.dll", "CreateIoCompletionPort");
    auto pGqcsEx  = (GqcsEx_t)kfn("kernel32.dll", "GetQueuedCompletionStatusEx");
    auto pQueueA2 = (QueueApc_t)kfn("kernel32.dll", "QueueUserAPC");
    static std::atomic<u64> g_iocp_apc{0};
    auto iocp_apc_cb = +[](u64 d) TRT_STDCALL -> void {
      g_iocp_apc.store(d, std::memory_order_release);
    };
    const u64 iocp = pIocp ? pIocp((void*)(uintptr_t)-1, 0, 0, 1) : 0;
    const u32 qa_ok = pQueueA2 ? pQueueA2((void*)iocp_apc_cb, (u64)(i64)-2, 0xCAFEBABEULL) : 0;
    u8 ov_entries[64] = {};
    u32 removed = 99;
    const int gq_rc = (iocp && pGqcsEx) ? pGqcsEx(iocp, ov_entries, 1, &removed, 50, 1) : -1;
    const u32 gq_err = trt_GetLastError();
    if (iocp) trt_CloseHandle((void*)(uintptr_t)iocp);
    CHECK(iocp != 0 && qa_ok == 1 && gq_rc == 0 && gq_err == 0xC0u /*WAIT_IO_COMPLETION*/ &&
              removed == 0 && g_iocp_apc.load() == 0xCAFEBABEULL,
          "Wave 6F: alertable GetQueuedCompletionStatusEx dispatches APC with WAIT_IO_COMPLETION");
  }

  // ---- Wave 6G: write-watch (bdwgc/IL2CPP incremental-GC probe surface) ----
  // bdwgc probes GetProcAddress("GetWriteWatch") + VirtualAlloc(MEM_WRITE_
  // WATCH|MEM_RESERVE|COMMIT, PAGE_READWRITE). Both must answer honestly.
  {
    auto kfn = [&](const char* mod, const char* sym) -> void* {
      return trt_resolve_module(nullptr, mod, sym, 0);
    };
    using GWWatch_t = u32(TRT_STDCALL*)(u32, void*, u64, void**, u64*, u32*);
    using RWW_t     = u32(TRT_STDCALL*)(void*, u64);
    using VirtualAlloc_t = void*(TRT_STDCALL*)(void*, u64, u32, u32);
    auto pGww = (GWWatch_t)kfn("kernel32.dll", "GetWriteWatch");
    auto pRww = (RWW_t)kfn("kernel32.dll", "ResetWriteWatch");
    auto pValloc = (VirtualAlloc_t)kfn("kernel32.dll", "VirtualAlloc");
    CHECK(pGww != nullptr && pRww != nullptr && pValloc != nullptr,
          "write-watch exports present in the bridge table");
    if (pGww && pValloc) {
      // the exact bdwgc probe: MEM_WRITE_WATCH (0x200000) | RESERVE|COMMIT
      constexpr u32 MEM_WW = 0x1000u | 0x2000u | 0x00200000u;
      void* gc = pValloc(nullptr, 1 << 20, MEM_WW, 0x04u /*PAGE_READWRITE*/);
      CHECK(gc != nullptr,
            "VirtualAlloc(MEM_WRITE_WATCH|RESERVE|COMMIT) accepted (GC probe lives)");
      if (gc) {
        volatile u8* bytes = (volatile u8*)gc;
        bytes[0 * 4096] = 1;  // touch page 0
        bytes[3 * 4096] = 2;  // touch page 3
        // enter a scope so the compiler keeps the volatile stores
        u64 n = 8;
        void* addrs[8] = {};
        u32 gran = 0;
        const u32 rc = pGww(0 /*no reset*/, gc, 1 << 20, addrs, &n, &gran);
        CHECK(rc == 0, "GetWriteWatch answers success (host passthrough or soft-dirty)");
        CHECK(n == 2 && addrs[0] == gc &&
                  addrs[1] == (void*)((u8*)gc + 3 * 4096),
              "GetWriteWatch reports exactly the 2 written pages, page-granular");
        // reset then re-query: clean means n == 0
        if (pRww) pRww(gc, 1 << 20);
        u64 n2 = 8;
        void* addrs2[8] = {};
        const u32 rc2 = pGww(0, gc, 1 << 20, addrs2, &n2, &gran);
        CHECK(rc2 == 0 && n2 == 0, "after ResetWriteWatch the region reports clean");
        pValloc(gc, 0, 0x8000u /*MEM_RELEASE*/, 0);
      }
    }
  }

  // ---- Sprint 1 gap batch: interlocked + Rtl memory + system info ----------
  // 1B/1C are NEW registrations (implemented fresh in bridge_kernel32.cpp);
  // 1D (GetSystemInfo, GetNativeSystemInfo, GetEnvironmentVariableW,
  // VirtualAllocEx, VirtualFreeEx) verifies the existing bridge_mem /
  // bridge_sys kernel32 table entries resolve. Every typedef carries
  // TRT_STDCALL: the bridges are ms_abi — resolving a pointer and calling it
  // through a SysV typedef passes garbage arguments.
  {
    auto kfn = [&](const char* mod, const char* sym) -> void* {
      return trt_resolve_module(nullptr, mod, sym, 0);
    };
    // (a) every name resolves through the module tables
    CHECK(kfn("kernel32.dll", "InterlockedIncrement") != nullptr &&
              kfn("kernel32.dll", "InterlockedDecrement") != nullptr &&
              kfn("kernel32.dll", "InterlockedExchangeAdd") != nullptr &&
              kfn("kernel32.dll", "InterlockedExchange") != nullptr &&
              kfn("kernel32.dll", "InterlockedCompareExchange") != nullptr &&
              kfn("kernel32.dll", "InterlockedIncrement64") != nullptr &&
              kfn("kernel32.dll", "InterlockedDecrement64") != nullptr &&
              kfn("kernel32.dll", "InterlockedExchangeAdd64") != nullptr &&
              kfn("kernel32.dll", "InterlockedCompareExchange64") != nullptr,
          "Sprint 1B: interlocked exports present in the kernel32 table");
    CHECK(kfn("kernel32.dll", "RtlCopyMemory") != nullptr &&
              kfn("kernel32.dll", "RtlMoveMemory") != nullptr &&
              kfn("kernel32.dll", "RtlZeroMemory") != nullptr &&
              kfn("kernel32.dll", "RtlFillMemory") != nullptr,
          "Sprint 1C: Rtl memory primitives present in the kernel32 table");
    CHECK(kfn("ntdll.dll", "RtlCopyMemory") != nullptr &&
              kfn("ntdll.dll", "RtlMoveMemory") != nullptr &&
              kfn("ntdll.dll", "RtlZeroMemory") != nullptr &&
              kfn("ntdll.dll", "RtlFillMemory") != nullptr,
          "Sprint 1C: Rtl memory primitives merged into the ntdll table");
    CHECK(kfn("kernel32.dll", "GetSystemInfo") != nullptr &&
              kfn("kernel32.dll", "GetNativeSystemInfo") != nullptr &&
              kfn("kernel32.dll", "GetEnvironmentVariableW") != nullptr &&
              kfn("kernel32.dll", "VirtualAllocEx") != nullptr &&
              kfn("kernel32.dll", "VirtualFreeEx") != nullptr,
          "Sprint 1D: system-info exports present in the kernel32 table");

    // (b) interlocked semantics: Increment/Decrement return the NEW value,
    // ExchangeAdd/CompareExchange return the OLD value
    using Inc_t   = i32(TRT_STDCALL*)(volatile i32*);
    using ExAdd_t = i32(TRT_STDCALL*)(volatile i32*, i32);
    using CmpEx_t = i32(TRT_STDCALL*)(volatile i32*, i32, i32);
    auto pInc   = (Inc_t)kfn("kernel32.dll", "InterlockedIncrement");
    auto pDec   = (Inc_t)kfn("kernel32.dll", "InterlockedDecrement");
    auto pExAdd = (ExAdd_t)kfn("kernel32.dll", "InterlockedExchangeAdd");
    auto pCmpEx = (CmpEx_t)kfn("kernel32.dll", "InterlockedCompareExchange");
    if (pInc && pExAdd && pDec && pCmpEx) {
      volatile i32 v = 41;
      CHECK(pInc(&v) == 42, "InterlockedIncrement returns the NEW value");
      CHECK(pExAdd(&v, 10) == 42 && v == 52,
            "InterlockedExchangeAdd returns the OLD value");
      CHECK(pDec(&v) == 51, "InterlockedDecrement returns the NEW value");
      CHECK(pCmpEx(&v, 99, 51) == 51 && v == 99,
            "InterlockedCompareExchange returns the OLD value and swaps on match");
    }
    using Inc64_t   = i64(TRT_STDCALL*)(volatile i64*);
    using ExAdd64_t = i64(TRT_STDCALL*)(volatile i64*, i64);
    auto pInc64   = (Inc64_t)kfn("kernel32.dll", "InterlockedIncrement64");
    auto pExAdd64 = (ExAdd64_t)kfn("kernel32.dll", "InterlockedExchangeAdd64");
    if (pInc64 && pExAdd64) {
      volatile i64 v64 = -2;
      CHECK(pInc64(&v64) == -1, "InterlockedIncrement64 returns the NEW 64-bit value");
      CHECK(pExAdd64(&v64, 7) == -1 && v64 == 6,
            "InterlockedExchangeAdd64 returns the OLD 64-bit value");
    }

    // (c) Rtl memory primitives copy/zero/fill bytes
    using RtlCopy_t = void(TRT_STDCALL*)(void*, const void*, u64);
    using RtlZero_t = void(TRT_STDCALL*)(void*, u64);
    using RtlFill_t = void(TRT_STDCALL*)(void*, u64, u8);
    auto pCopy = (RtlCopy_t)kfn("kernel32.dll", "RtlCopyMemory");
    auto pMove = (RtlCopy_t)kfn("kernel32.dll", "RtlMoveMemory");
    auto pZero = (RtlZero_t)kfn("kernel32.dll", "RtlZeroMemory");
    auto pFill = (RtlFill_t)kfn("kernel32.dll", "RtlFillMemory");
    if (pCopy && pZero && pFill) {
      u8 ra[16], rb[16];
      for (int i = 0; i < 16; ++i) ra[i] = (u8)(0x40 + i);
      memset(rb, 0xEE, sizeof(rb));
      pCopy(rb, ra, sizeof(ra));
      bool copied = true;
      for (int i = 0; i < 16; ++i) copied = copied && rb[i] == (u8)(0x40 + i);
      CHECK(copied, "RtlCopyMemory copies bytes");
      pZero(rb, sizeof(rb));
      bool zeroed = true;
      for (int i = 0; i < 16; ++i) zeroed = zeroed && rb[i] == 0;
      CHECK(zeroed, "RtlZeroMemory zeroes the buffer");
      pFill(rb, sizeof(rb), 0xA5);
      bool filled = true;
      for (int i = 0; i < 16; ++i) filled = filled && rb[i] == 0xA5;
      CHECK(filled, "RtlFillMemory fills the fill byte");
    }
    if (pMove) {
      u8 ra[8], rb[16];
      for (int i = 0; i < 8; ++i) ra[i] = (u8)(0x60 + i);
      memset(rb, 0xC3, sizeof(rb));
      pMove(rb, ra, sizeof(ra));
      CHECK(rb[0] == 0x60 && rb[7] == 0x67 && rb[8] == 0xC3,
            "RtlMoveMemory copies exactly the requested bytes");
    }

    // (d) GetSystemInfo fills a plausible SYSTEM_INFO (Win64 layout)
    using GetSysInfo_t = void(TRT_STDCALL*)(void*);
    auto pGsi  = (GetSysInfo_t)kfn("kernel32.dll", "GetSystemInfo");
    auto pGnsi = (GetSysInfo_t)kfn("kernel32.dll", "GetNativeSystemInfo");
    if (pGsi) {
      u8 si[48] = {};
      pGsi(si);
      const u16 arch  = *(u16*)(si + 0);
      const u32 page  = *(u32*)(si + 4);
      const u32 nproc = *(u32*)(si + 0x20);
      const u32 gran  = *(u32*)(si + 0x28);
      CHECK(arch == 9 /*PROCESSOR_ARCHITECTURE_AMD64*/ && page == 4096 &&
                nproc >= 1 && gran == 65536,
            "GetSystemInfo fills a plausible SYSTEM_INFO (page 4096, procs >= 1)");
      if (pGnsi) {
        pGnsi(si);
        CHECK(*(u32*)(si + 4) == 4096,
              "GetNativeSystemInfo matches (no WOW64 downgrade)");
      }
    }

    // (e) GetEnvironmentVariableW round-trips a variable set through the
    // kernel32 SetEnvironmentVariableW store (the PyInstaller onefile path)
    using GetEnvW_t = u32(TRT_STDCALL*)(const u16*, u16*, u32);
    using SetEnvW_t = int(TRT_STDCALL*)(const u16*, const u16*);
    auto pGetEnv = (GetEnvW_t)kfn("kernel32.dll", "GetEnvironmentVariableW");
    auto pSetEnv = (SetEnvW_t)kfn("kernel32.dll", "SetEnvironmentVariableW");
    if (pGetEnv && pSetEnv) {
      u16 wname[32], wval[32], wbuf[64];
      make_u16("TRT_SPRINT1_ENV", wname, 32);
      make_u16("gap_batch_ok", wval, 32);
      CHECK(pSetEnv(wname, wval) == 1,
            "SetEnvironmentVariableW accepts the value");
      const u32 need = pGetEnv(wname, nullptr, 0);
      CHECK(need == 13 /*strlen("gap_batch_ok")+1*/,
            "GetEnvironmentVariableW returns a set variable's length");
      const u32 got = pGetEnv(wname, wbuf, 64);
      bool val_ok = (got == need);
      for (u32 i = 0; val_ok && i + 1 < got; ++i) val_ok = (wbuf[i] == wval[i]);
      CHECK(val_ok, "GetEnvironmentVariableW reads the value back");
      pSetEnv(wname, nullptr);  // clean the store
    }

    // (f) VirtualAllocEx/VirtualFreeEx accept the pseudo current-process
    // handle and behave like VirtualAlloc/VirtualFree
    using VAllocEx_t = void*(TRT_STDCALL*)(void*, void*, u64, u32, u32);
    using VFreeEx_t  = int(TRT_STDCALL*)(void*, void*, u64, u32);
    auto pVax = (VAllocEx_t)kfn("kernel32.dll", "VirtualAllocEx");
    auto pVfx = (VFreeEx_t)kfn("kernel32.dll", "VirtualFreeEx");
    if (pVax && pVfx) {
      void* vmem = pVax((void*)(uintptr_t)-1 /*pseudo current process*/,
                        nullptr, 65536, 0x3000u /*RESERVE|COMMIT*/,
                        0x04u /*PAGE_READWRITE*/);
      CHECK(vmem != nullptr,
            "VirtualAllocEx(pseudo process) commits like VirtualAlloc");
      if (vmem) {
        CHECK(pVfx((void*)(uintptr_t)-1, vmem, 0, 0x8000u /*MEM_RELEASE*/) == 1,
              "VirtualFreeEx(pseudo process) releases the region");
      }
      trt_SetLastError(0);
      void* bad = pVax((void*)(uintptr_t)0xBEEF0000, nullptr, 65536,
                       0x3000u, 0x04u);
      CHECK(bad == nullptr && trt_GetLastError() == 5u /*ACCESS_DENIED*/,
            "VirtualAllocEx rejects a foreign process handle");
    }
  }

  printf("\nRESULT: %s\n", g_failures ? "FAIL" : "ALL PASS");
  return g_failures ? 1 : 0;
}
