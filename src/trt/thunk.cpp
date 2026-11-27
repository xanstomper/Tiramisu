// tiramisu/src/trt/thunk.cpp
// Per-symbol fail-visible thunks (see thunk.h).
//
// The dispatcher uses the guest ABI. A tail jump reuses the caller's
// return address and shadow space, so the generated code stays a leaf and
// never needs its own unwind metadata.
#include "thunk.h"

#include <stdio.h>
#include <string.h>

#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr size_t k_thunk_stride = 64;
constexpr size_t k_thunks_per_page = 4096 / k_thunk_stride; // 64 slots per 4 KiB page

struct thunk_entry {
  std::string kind;
  std::string mod;
  std::string sym;
  void* code = nullptr;
  u64 ret_val = 0;
  u64 launch_generation = 0;
  unsigned logged_calls = 0;
};

std::mutex g_thunk_mu;
std::deque<thunk_entry> g_thunks;
std::unordered_map<std::string, size_t> g_thunk_map;
std::vector<unsigned char*> g_thunk_pages;
u64 g_launch_generation = 1;
trt_unsupported_api_call g_first_call{};

std::mutex g_backend_mu;
trt_backend g_backend;
bool g_backend_set = false;

#if defined(_WIN32) || (defined(__x86_64__) && defined(__clang__))
const unsigned char k_stub_arg_mov[] = {0x48, 0xB9};  // mov rcx, imm64
#else
const unsigned char k_stub_arg_mov[] = {0x48, 0xBF};  // mov rdi, imm64
#endif
const unsigned char k_stub_mov_r11[] = {0x49, 0xBB}; // mov r11, imm64
const unsigned char k_stub_jmp_r11[] = {0x41, 0xFF, 0xE3}; // jmp r11

void put_u64(unsigned char* p, u64 v) {
  for (int i = 0; i < 8; ++i) p[i] = (unsigned char)((v >> (i * 8)) & 0xFF);
}

void write_stub_slot(unsigned char* code, u64 index, u64 dispatch_addr) {
  unsigned char* p = code;
  memcpy(p, k_stub_arg_mov, sizeof(k_stub_arg_mov)); p += sizeof(k_stub_arg_mov);
  put_u64(p, index); p += 8;
  memcpy(p, k_stub_mov_r11, sizeof(k_stub_mov_r11)); p += sizeof(k_stub_mov_r11);
  put_u64(p, dispatch_addr); p += 8;
  memcpy(p, k_stub_jmp_r11, sizeof(k_stub_jmp_r11));
}

} // namespace

extern "C" {

void trt_core_set_backend(const trt_backend* be) {
  if (!be) return;
  std::lock_guard<std::mutex> lk(g_backend_mu);
  g_backend = *be;
  g_backend_set = true;
}

const trt_backend* trt_core_backend(void) {
  std::lock_guard<std::mutex> lk(g_backend_mu);
  return g_backend_set ? &g_backend : nullptr;
}

void trt_core_log(const char* line) {
  void (*log_fn)(const char*) = nullptr;
  {
    std::lock_guard<std::mutex> lk(g_backend_mu);
    if (g_backend_set && g_backend.log) log_fn = g_backend.log;
  }
  // .203 diagnostic: tag every log line with the calling thread id, so
  // Sekiro/Ultrapool stalls can be attributed (main vs worker). The
  // function does not log, so there is no recursion risk.
  char tagged[2048];
  snprintf(tagged, sizeof(tagged), "[tid=%u] %s",
           trt_GetCurrentThreadId(), line);
  if (log_fn) log_fn(tagged);
  else fprintf(stderr, "  [trt] %s\n", tagged);
}

void trt_thunk_begin_launch(void) {
  std::lock_guard<std::mutex> lk(g_thunk_mu);
  ++g_launch_generation;
  g_first_call = {};
}

trt_unsupported_api_call trt_thunk_first_call(void) {
  std::lock_guard<std::mutex> lk(g_thunk_mu);
  return g_first_call;
}

u64 TRT_GUEST_ABI trt_thunk_dispatch(u64 index) {
  char line[512]{};
  u64 ret = 0;
  {
    std::lock_guard<std::mutex> lk(g_thunk_mu);
    if (index < (u64)g_thunks.size()) {
      thunk_entry& e = g_thunks[(size_t)index];
      ret = e.ret_val;
      if (e.launch_generation != g_launch_generation) {
        e.launch_generation = g_launch_generation;
        e.logged_calls = 0;
      }
      if (g_first_call.observed_calls++ == 0) {
        g_first_call.thread_id = trt_GetCurrentThreadId();
        g_first_call.return_value = ret;
        snprintf(g_first_call.kind, sizeof(g_first_call.kind), "%s", e.kind.c_str());
        snprintf(g_first_call.name, sizeof(g_first_call.name), "%s!%s",
                 e.mod.c_str(), e.sym.c_str());
      }
      if (e.logged_calls < 6) {
        ++e.logged_calls;
        snprintf(line, sizeof(line),
                 "trt: %s %s!%s called (unimplemented) -> ret=0x%llx",
                 e.kind.c_str(), e.mod.c_str(), e.sym.c_str(), (unsigned long long)ret);
      }
    } else {
      snprintf(line, sizeof(line), "trt: thunk called with a bad index");
    }
  }
  if (line[0]) trt_core_log(line);
  return ret;
}

void* trt_thunk_make_ext(const trt_backend* be, const char* kind, const char* mod,
                         const char* sym, u64 ret_val) {
  if (!kind) kind = "STUB";
  if (!mod) mod = "?";
  if (!sym) sym = "?";

  std::string key;
  key.reserve(strlen(kind) + strlen(mod) + strlen(sym) + 2);
  key.append(kind).push_back('\x1f');
  key.append(mod).push_back('\x1f');
  key.append(sym);

  const trt_backend* backend = be ? be : trt_core_backend();
  void* created_code = nullptr;
  {
    std::lock_guard<std::mutex> lk(g_thunk_mu);
    auto it = g_thunk_map.find(key);
    if (it != g_thunk_map.end()) {
      g_thunks[it->second].ret_val = ret_val;
      return g_thunks[it->second].code;
    }

    if (!backend || !backend->alloc || !backend->protect || !backend->release) {
      return nullptr;
    }

    const size_t index = g_thunks.size();
    const size_t page_idx = index / k_thunks_per_page;
    const size_t slot_idx = index % k_thunks_per_page;

    if (page_idx >= g_thunk_pages.size()) {
      // Pre-populate all 64 slots on a fresh 4 KiB page while RW, then flip RX once.
      unsigned char* page = (unsigned char*)backend->alloc(4096, 0);
      if (!page) return nullptr;
      const u64 base_idx = (u64)page_idx * k_thunks_per_page;
      const u64 dispatch_addr = (u64)(uintptr_t)&trt_thunk_dispatch;
      for (size_t s = 0; s < k_thunks_per_page; ++s) {
        write_stub_slot(page + s * k_thunk_stride, base_idx + s, dispatch_addr);
      }
      if (backend->protect(page, 4096, TRT_PROT_R | TRT_PROT_X) != 0) {
        backend->release(page, 4096);
        return nullptr;
      }
      g_thunk_pages.push_back(page);
    }

    created_code = g_thunk_pages[page_idx] + slot_idx * k_thunk_stride;
    thunk_entry e;
    e.kind = kind;
    e.mod = mod;
    e.sym = sym;
    e.code = created_code;
    e.ret_val = ret_val;
    g_thunks.push_back(std::move(e));
    g_thunk_map.emplace(std::move(key), index);
  }

  char line[512];
  snprintf(line, sizeof(line), "trt: %s %s!%s -> fail-visible thunk (default ret=0x%llx)",
           kind, mod, sym, (unsigned long long)ret_val);
  trt_core_log(line);
  return created_code;
}

void* trt_thunk_make(const trt_backend* be, const char* kind, const char* mod,
                     const char* sym) {
  return trt_thunk_make_ext(be, kind, mod, sym, 0);
}

} // extern "C"
