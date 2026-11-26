// tiramisu/src/trt/bridge_mem.cpp
// Streaming windowed memory-mapped files for the guest.
//
// Commercial games map multi-GB asset archives (Unity bundles, Unreal .pak,
// custom containers) and inspect rolling windows via CreateFileMapping +
// MapViewOfFile(offset, bytes) + UnmapViewOfFile. On console AppContainer,
// cross-process file-mapping permissions can be restricted, so a view is never
// shared with another process. Two backing strategies, chosen per view:
//
//   file (default, read-only): the view IS the file's clean page cache —
//     mmap(PROT_READ, MAP_PRIVATE) over the host descriptor. The SSD and RAM
//     overlap: the window costs address space instead of committed RAM, the
//     kernel drops its pages under pressure for free and the next touch
//     re-faults them from the SSD. Read-only has no divergent state to lose.
//   copy (write/COW views, views past EOF, TRT_MAP_MODE=copy): a private
//     allocation populated for [offset, offset+len) via 64-bit positional I/O,
//     released on UnmapViewOfFile (write-back when the view is dirty).
#include "trt.h"
#include "thunk.h"
#include "bridge_mem.h"

#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <string.h>
#include <vector>

#ifdef __linux__
#include <sys/mman.h>
#include <unistd.h>
#endif

extern "C" void TRT_STDCALL trt_SetLastError(u32 e);
// 5 GB memory-budget governor (bridge_membudget.cpp): mapped views charge
// the same ledger as VirtualAlloc/heap commit — over-budget maps refuse.
extern "C" int trt_mem_budget_charge_map(u64 bytes);
extern "C" void trt_mem_budget_discharge_map(u64 bytes);
// the anonymous pagefile backing buffer is real committed memory too
extern "C" int trt_mem_budget_charge_commit(u64 bytes);
extern "C" void trt_mem_budget_discharge_commit(u64 bytes);
extern "C" int trt_mem_budget_charge_va(u64 bytes);
extern "C" void trt_mem_budget_discharge_va(u64 bytes);
// File-backed read-only views: the view's pages ARE the file's clean page
// cache, so the mapping charges VA only — never commit.
extern "C" int trt_mem_budget_charge_file_view(u64 bytes);
extern "C" void trt_mem_budget_discharge_file_view(u64 bytes);
// host descriptor + size behind a guest file handle (bridge_kernel32.cpp)
extern "C" int trt_file_host_fd(void* handle, u64* size_out);
extern "C" const char* trt_file_vpath(void* h);

namespace {

constexpr int PROT_READ_ = 0x01;
constexpr int PROT_WRITE_ = 0x02;
constexpr int PROT_EXEC_ = 0x04;
constexpr u32 k_map_handle_tag = 0x5A000000u;
constexpr u32 k_map_handle_mask = 0xFFF00000u;
constexpr u32 k_err_file_not_found = 2;
constexpr u32 k_err_invalid_handle = 6;
constexpr u32 k_err_invalid_parameter = 87;
constexpr u32 k_err_already_exists = 183;
constexpr u32 k_err_file_invalid = 1006;

struct mapping_core {
  void* file_handle = nullptr;  // retained VFS handle, or nullptr for anon
  u8* anon_buf = nullptr;
  u64 anon_cap = 0;
  u64 size = 0;
  u32 protect = 0x02;           // Win32 PAGE_*
  u32 handle_refs = 0;
  u32 view_refs = 0;
  std::string name;
};

struct mapping_handle {
  mapping_core* core = nullptr;
  bool used = false;
};

struct mapped_view {
  void* ptr = nullptr;
  size_t bytes = 0;
  u64 offset = 0;
  mapping_core* core = nullptr;
  bool flush_on_unmap = false;
  bool from_be = false;
  u64 stamp = 0;  // LRU recency stamp (g_map_clock at last registry touch)
  // File-backed read-only views (map, don't copy): map_base/map_span are the
  // munmap target, charge_span is exactly what the budget ledger was charged
  // (the mapping span, which exceeds bytes when the offset is page-misaligned).
  bool file_backed = false;
  void* map_base = nullptr;
  size_t map_span = 0;
  u64 charge_span = 0;
};

std::mutex g_mem_mu;
std::vector<mapping_handle> g_handles;
std::vector<mapping_core*> g_cores;
std::vector<mapped_view> g_views;

// Sprint 2 item 35: live-view ceiling + LRU eviction. The registry IS
// g_views (every map inserts, every unmap removes). The clock ticks once per
// registry touch (map / flush / unmap), so stamp order == recency order.
u64 g_map_clock = 0;

// Ceiling: 64 live views by default, TRT_MAX_MAPS overrides (parsed once,
// clamped 8..4096). Streaming games that leak views hit this valve instead of
// eating the console's whole memory budget.
unsigned view_ceiling() {
  static const unsigned ceiling = [] {
    unsigned v = 64;
    const char* s = std::getenv("TRT_MAX_MAPS");
    if (s && s[0]) {
      char* end = nullptr;
      const long n = std::strtol(s, &end, 10);
      if (end && end != s && n > 0) v = (unsigned)n;
    }
    if (v < 8) v = 8;
    if (v > 4096) v = 4096;
    return v;
  }();
  return ceiling;
}

void log_ceiling_once() {
  static bool done = false;
  if (done) return;
  done = true;
  char line[96];
  snprintf(line, sizeof(line), "[trt] mem: view ceiling = %u (TRT_MAX_MAPS=%s)",
           view_ceiling(),
           std::getenv("TRT_MAX_MAPS") ? std::getenv("TRT_MAX_MAPS") : "unset");
  trt_core_log(line);
}

// Use-after-evict detection: an evicted view's pages return to the runtime
// (the backing allocation is released), so a guest pointer into it is dead.
// The bridge cannot trap raw guest reads/writes through the stale pointer
// (backend-allocated views fault visibly; calloc-backed ones are ordinary
// freed heap), but any later bridge call through the stale pointer is named
// ONCE per evicted range instead of being silently ignored.
struct evicted_range {
  uintptr_t ptr;
  size_t bytes;
  u64 offset;
  u64 age;       // map-clock age at eviction
  bool reported; // the use-after-evict line is logged once per range
};
std::vector<evicted_range> g_evicted;  // bounded ring: oldest dropped past 128

size_t find_evicted_idx_locked(uintptr_t va) {
  for (size_t i = 0; i < g_evicted.size(); ++i) {
    if (va >= g_evicted[i].ptr && va < g_evicted[i].ptr + g_evicted[i].bytes) {
      return i;
    }
  }
  return SIZE_MAX;
}

// Returns true (and logs once) when the pointer falls inside an evicted view.
bool hit_evicted_locked(void* view, const char* op) {
  const size_t ei = find_evicted_idx_locked((uintptr_t)view);
  if (ei == SIZE_MAX) return false;
  evicted_range& r = g_evicted[ei];
  if (!r.reported) {
    r.reported = true;
    char line[224];
    snprintf(line, sizeof(line),
             "[trt] mem: use-after-evict detected: %s of ptr=%p hits evicted "
             "view off=0x%llx len=%llu (age %llu maps) -> view is gone",
             op, view, (unsigned long long)r.offset,
             (unsigned long long)r.bytes, (unsigned long long)r.age);
    trt_core_log(line);
  }
  return true;
}

std::string normalize_map_name_u16(const u16* w) {
  if (!w || !w[0]) return {};
  char utf8[512] = {};
  if (trt_utf16_to_utf8(w, utf8, sizeof(utf8)) != 0 || !utf8[0]) return {};
  for (char* p = utf8; *p; ++p) {
    if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
  }
  return std::string(utf8);
}

std::string normalize_map_name_a(const char* s) {
  if (!s || !s[0]) return {};
  std::string out(s);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  }
  return out;
}

mapping_core* find_named_core_locked(const std::string& name) {
  if (name.empty()) return nullptr;
  for (mapping_core* c : g_cores) {
    if (c && c->name == name) return c;
  }
  return nullptr;
}

u64 alloc_handle_locked(mapping_core* core) {
  for (size_t i = 0; i < g_handles.size(); ++i) {
    if (!g_handles[i].used) {
      g_handles[i].used = true;
      g_handles[i].core = core;
      ++core->handle_refs;
      return k_map_handle_tag | (u32)i;
    }
  }
  if (g_handles.size() >= 0xFFFFFu) return 0;
  mapping_handle h{};
  h.used = true;
  h.core = core;
  ++core->handle_refs;
  const u32 idx = (u32)g_handles.size();
  g_handles.push_back(h);
  return k_map_handle_tag | idx;
}

void maybe_destroy_core_locked(mapping_core* core) {
  if (!core || core->handle_refs > 0 || core->view_refs > 0) return;
  if (core->file_handle) {
    trt_file_release(core->file_handle);
    core->file_handle = nullptr;
  }
  if (core->anon_buf) {
    free(core->anon_buf);
    core->anon_buf = nullptr;
    trt_mem_budget_discharge_commit(core->anon_cap);
    trt_mem_budget_discharge_va(core->anon_cap);
    core->anon_cap = 0;
  }
  for (size_t i = 0; i < g_cores.size(); ++i) {
    if (g_cores[i] == core) {
      g_cores.erase(g_cores.begin() + (ptrdiff_t)i);
      break;
    }
  }
  delete core;
}

mapping_core* core_from_handle_locked(u64 h, u32* idx_out = nullptr) {
  if ((h & k_map_handle_mask) != k_map_handle_tag) return nullptr;
  const u32 idx = (u32)(h & 0xFFFFFu);
  if (idx >= g_handles.size() || !g_handles[idx].used) return nullptr;
  if (idx_out) *idx_out = idx;
  return g_handles[idx].core;
}

int map_access_to_prot_bits(u32 access, u32 sec_protect) {
  int bits = PROT_READ_;
  const bool want_write = (access & 0x02u) != 0 || (access == 0xF001Fu) ||
                          (access & 0x01u) != 0;
  const bool want_exec = (access & 0x20u) != 0 ||
                         ((sec_protect & 0xF0u) != 0);
  if (want_write) bits |= PROT_WRITE_;
  if (want_exec) bits |= PROT_EXEC_;
  return bits;
}

unsigned prot_bits_to_trt(int bits) {
  unsigned p = 0;
  if (bits & PROT_READ_) p |= TRT_PROT_R;
  if (bits & PROT_WRITE_) p |= TRT_PROT_W;
  if (bits & PROT_EXEC_) p |= TRT_PROT_X;
  return p ? p : TRT_PROT_R;
}

void* alloc_view_buf(size_t bytes, bool* out_from_be) {
  const trt_backend* be = trt_core_backend();
  if (be && be->alloc && be->release) {
    void* p = be->alloc(bytes, 0);
    if (p) {
      *out_from_be = true;
      return p;
    }
  }
  *out_from_be = false;
  return calloc(1, bytes);
}

void free_view_buf(void* p, size_t bytes, bool from_be) {
  if (!p) return;
  const trt_backend* be = trt_core_backend();
  if (from_be && be && be->release) {
    be->release(p, bytes);
    return;
  }
  free(p);
}

// ---------------------------------------------------------------------------
// Read-only views over real files are MAPPED, not copied. The kernel already
// holds the bytes in the page cache, so the view costs address space instead of
// committed RAM; clean pages are reclaimable for free under pressure and the
// next touch re-faults them from the SSD rather than failing an allocation.
// That is the SSD/RAM overlap: a multi-GB asset archive streams through the
// 5 GB budget instead of being copied into it. TRT_MAP_MODE=copy restores the
// private-copy path for diagnosis; write/COW views always copy.
// ---------------------------------------------------------------------------

bool file_view_mode_enabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("TRT_MAP_MODE");
    if (env && strcmp(env, "copy") == 0) return false;
    return true;
  }();
  return enabled;
}

bool file_view_readahead_enabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("TRT_MAP_READAHEAD");
    return !(env && strcmp(env, "0") == 0);
  }();
  return enabled;
}

void log_file_view_once() {
  static bool done = false;
  if (done) return;
  done = true;
  trt_core_log("trt: mem: read-only map mode = file — views are file-backed "
               "page cache (mapped, not copied; clean pages reclaimable, "
               "re-faulted from the SSD; TRT_MAP_MODE=copy restores the copy "
               "path)");
}

// Discharge exactly what the view charged: file-backed views charged VA only
// (charge_span), copy views charged the full map ledger (bytes).
void release_view_charge_locked(const mapped_view& mv) {
  if (mv.file_backed) {
    trt_mem_budget_discharge_file_view(mv.charge_span);
    return;
  }
  trt_mem_budget_discharge_map((u64)mv.bytes);
}

void release_view_backing_locked(const mapped_view& mv) {
  if (mv.file_backed) {
#ifdef __linux__
    if (mv.map_base && mv.map_span) munmap(mv.map_base, mv.map_span);
#endif
    return;
  }
  free_view_buf(mv.ptr, mv.bytes, mv.from_be);
}

bool flush_view_locked(const mapped_view& v, size_t sub_off, size_t len) {
  if (!v.flush_on_unmap || !v.core || len == 0) return true;
  const u8* src = (const u8*)v.ptr + sub_off;
  const u64 core_off = v.offset + sub_off;
  if (v.core->file_handle) {
    size_t put = 0;
    return trt_file_pwrite(v.core->file_handle, core_off, src, len, &put) != 0;
  }
  if (v.core->anon_buf && core_off < v.core->anon_cap) {
    size_t copy_len = len;
    if (core_off + copy_len > v.core->anon_cap) {
      copy_len = (size_t)(v.core->anon_cap - core_off);
    }
    memcpy(v.core->anon_buf + core_off, src, copy_len);
  }
  return true;
}

// Sprint 2 item 35: evict one view — pages return to the runtime, the range
// is recorded for use-after-evict detection, registry bookkeeping unwinds.
mapped_view evict_view_locked(size_t idx) {
  const mapped_view mv = g_views[idx];
  g_views.erase(g_views.begin() + (ptrdiff_t)idx);
  if (g_evicted.size() >= 128) g_evicted.erase(g_evicted.begin());
  g_evicted.push_back({(uintptr_t)mv.ptr, mv.bytes, mv.offset,
                       g_map_clock - mv.stamp, false});
  release_view_charge_locked(mv);
  release_view_backing_locked(mv);
  if (mv.core && mv.core->view_refs > 0) --mv.core->view_refs;
  maybe_destroy_core_locked(mv.core);
  return mv;
}

// Called on a map that would exceed the ceiling. Evicts the LEAST-RECENTLY-
// USED view, preferring CLEAN views (flush_on_unmap == false — nothing
// pending). If every live view is dirty, flush-then-evict the LRU one
// (flush_view_locked persists the pending bytes first, so no data is lost);
// a view whose flush FAILS is unevictable and is skipped. Returns false only
// when nothing could be evicted — the caller fails the map honestly.
bool evict_lru_for_map_locked(unsigned ceiling) {
  size_t best = SIZE_MAX;
  u64 best_stamp = ~0ull;
  for (size_t i = 0; i < g_views.size(); ++i) {
    if (g_views[i].flush_on_unmap) continue;  // potentially dirty
    if (g_views[i].stamp < best_stamp) {
      best_stamp = g_views[i].stamp;
      best = i;
    }
  }
  if (best != SIZE_MAX) {
    const mapped_view mv = evict_view_locked(best);
    char line[224];
    snprintf(line, sizeof(line),
             "[trt] mem: map ceiling %u hit — evicting LRU view ptr=%p "
             "off=0x%llx len=%llu (age %llu maps)",
             ceiling, mv.ptr, (unsigned long long)mv.offset,
             (unsigned long long)mv.bytes,
             (unsigned long long)(g_map_clock - mv.stamp));
    trt_core_log(line);
    return true;
  }

  // Every live view is dirty: flush-then-evict in LRU order. A flush failure
  // means the pending data cannot be persisted — that view is unevictable.
  std::vector<const void*> skipped;
  for (size_t tries = g_views.size(); tries > 0; --tries) {
    size_t best_d = SIZE_MAX;
    u64 best_d_stamp = ~0ull;
    for (size_t i = 0; i < g_views.size(); ++i) {
      bool was_skipped = false;
      for (const void* p : skipped) {
        if (p == g_views[i].ptr) {
          was_skipped = true;
          break;
        }
      }
      if (was_skipped) continue;
      if (g_views[i].stamp < best_d_stamp) {
        best_d_stamp = g_views[i].stamp;
        best_d = i;
      }
    }
    if (best_d == SIZE_MAX) break;
    mapped_view& v = g_views[best_d];
    if (flush_view_locked(v, 0, v.bytes)) {
      const mapped_view mv = evict_view_locked(best_d);
      char line[256];
      snprintf(line, sizeof(line),
               "[trt] mem: map ceiling %u hit — evicting LRU dirty view "
               "ptr=%p off=0x%llx len=%llu (age %llu maps, flushed first)",
               ceiling, mv.ptr, (unsigned long long)mv.offset,
               (unsigned long long)mv.bytes,
               (unsigned long long)(g_map_clock - mv.stamp));
      trt_core_log(line);
      return true;
    }
    char line[192];
    snprintf(line, sizeof(line),
             "[trt] mem: ceiling eviction: flush of dirty view ptr=%p "
             "off=0x%llx failed — unevictable, skipping",
             v.ptr, (unsigned long long)v.offset);
    trt_core_log(line);
    skipped.push_back(v.ptr);
  }
  return false;
}

u64 create_mapping_common(u64 hfile, u32 protect, u64 max_size,
                          const std::string& norm_name) {
  std::lock_guard<std::mutex> lk(g_mem_mu);
  if (!norm_name.empty()) {
    if (mapping_core* existing = find_named_core_locked(norm_name)) {
      const u64 h = alloc_handle_locked(existing);
      trt_SetLastError(h ? k_err_already_exists : k_err_invalid_parameter);
      return h;
    }
  }

  const bool is_anon = (hfile == (u64)-1 || hfile == 0);
  u64 file_sz = 0;
  if (!is_anon) {
    void* fh = (void*)(uintptr_t)hfile;
    if (!trt_file_stat_size(fh, &file_sz)) {
      trt_SetLastError(k_err_invalid_handle);
      return 0;
    }
    if (file_sz == 0 && max_size == 0) {
      trt_SetLastError(k_err_file_invalid);
      return 0;
    }
    if (!trt_file_retain(fh)) {
      trt_SetLastError(k_err_invalid_handle);
      return 0;
    }
  }

  mapping_core* core = new mapping_core();
  core->file_handle = is_anon ? nullptr : (void*)(uintptr_t)hfile;
  core->size = max_size ? max_size : (is_anon ? 0x1000ull : file_sz);
  core->protect = protect;
  core->name = norm_name;
  g_cores.push_back(core);

  const u64 h = alloc_handle_locked(core);
  if (!h) {
    maybe_destroy_core_locked(core);
    trt_SetLastError(k_err_invalid_parameter);
    return 0;
  }
  trt_SetLastError(0);

  char line[256];
  const char* vp = trt_file_vpath((void*)(uintptr_t)hfile);
  snprintf(line, sizeof(line),
           "trt: CreateFileMapping%s(hfile=%p '%s') streaming size=%llu",
           norm_name.empty() ? "" : "Named",
           (void*)(uintptr_t)hfile, vp ? vp : "", (unsigned long long)core->size);
  trt_core_log(line);
  return h;
}

}  // namespace

extern "C" {

int trt_mem_close_handle(void* h) {
  std::lock_guard<std::mutex> lk(g_mem_mu);
  u32 idx = 0;
  mapping_core* core = core_from_handle_locked((u64)(uintptr_t)h, &idx);
  if (!core) return 0;
  g_handles[idx].used = false;
  g_handles[idx].core = nullptr;
  if (core->handle_refs > 0) --core->handle_refs;
  maybe_destroy_core_locked(core);
  return 1;
}

void trt_mem_stats(u32* active_mappings, u32* active_views,
                   u64* total_view_bytes) {
  std::lock_guard<std::mutex> lk(g_mem_mu);
  if (active_mappings) *active_mappings = (u32)g_cores.size();
  if (active_views) *active_views = (u32)g_views.size();
  if (total_view_bytes) {
    u64 sum = 0;
    for (const mapped_view& v : g_views) sum += v.bytes;
    *total_view_bytes = sum;
  }
}

u64 TRT_STDCALL trt_CreateFileMappingW(u64 hfile, void* attrs, u32 protect,
                                       u32 max_high, u32 max_low,
                                       const u16* name) {
  (void)attrs;
  const u64 max_size = ((u64)max_high << 32) | max_low;
  return create_mapping_common(hfile, protect, max_size,
                               normalize_map_name_u16(name));
}

u64 TRT_STDCALL trt_CreateFileMappingA(u64 hfile, void* attrs, u32 protect,
                                       u32 max_high, u32 max_low,
                                       const char* name) {
  (void)attrs;
  const u64 max_size = ((u64)max_high << 32) | max_low;
  return create_mapping_common(hfile, protect, max_size,
                               normalize_map_name_a(name));
}

u64 TRT_STDCALL trt_CreateFileMappingNumaW(u64 hfile, void* attrs, u32 protect,
                                           u32 max_high, u32 max_low,
                                           const u16* name, u32 preferred) {
  (void)preferred;
  return trt_CreateFileMappingW(hfile, attrs, protect, max_high, max_low, name);
}

u64 TRT_STDCALL trt_OpenFileMappingW(u32 access, int inherit, const u16* name) {
  (void)access;
  (void)inherit;
  const std::string norm = normalize_map_name_u16(name);
  std::lock_guard<std::mutex> lk(g_mem_mu);
  mapping_core* core = find_named_core_locked(norm);
  if (!core) {
    trt_SetLastError(k_err_file_not_found);
    return 0;
  }
  trt_SetLastError(0);
  return alloc_handle_locked(core);
}

u64 TRT_STDCALL trt_OpenFileMappingA(u32 access, int inherit,
                                     const char* name) {
  (void)access;
  (void)inherit;
  const std::string norm = normalize_map_name_a(name);
  std::lock_guard<std::mutex> lk(g_mem_mu);
  mapping_core* core = find_named_core_locked(norm);
  if (!core) {
    trt_SetLastError(k_err_file_not_found);
    return 0;
  }
  trt_SetLastError(0);
  return alloc_handle_locked(core);
}

void* TRT_STDCALL trt_MapViewOfFileEx(u64 h, u32 access, u32 off_high,
                                      u32 off_low, u64 bytes, void* base_addr) {
  (void)base_addr;
  std::lock_guard<std::mutex> lk(g_mem_mu);
  mapping_core* core = core_from_handle_locked(h);
  if (!core) {
    trt_SetLastError(k_err_invalid_handle);
    return nullptr;
  }
  const u64 offset = ((u64)off_high << 32) | off_low;
  if (core->size > 0 && offset >= core->size) {
    trt_SetLastError(k_err_invalid_parameter);
    return nullptr;
  }
  u64 req_len = bytes ? bytes : (core->size > offset ? core->size - offset : 0);
  if (core->size > 0 && offset + req_len > core->size) {
    req_len = core->size - offset;
  }
  if (req_len == 0) req_len = 0x1000ull;

  // Sprint 2 item 35: live-view ceiling. Before allocating another window,
  // make room by evicting the least-recently-used view (clean preferred;
  // dirty views are flushed first so no pending data is lost). If nothing
  // can be evicted the map fails honestly — never silent, never a crash.
  const unsigned ceiling = view_ceiling();
  log_ceiling_once();
  if (g_views.size() + 1 > (size_t)ceiling &&
      !evict_lru_for_map_locked(ceiling)) {
    char line[192];
    snprintf(line, sizeof(line),
             "[trt] mem: map ceiling %u hit — no evictable view -> "
             "MapViewOfFile fails (ERROR_NOT_ENOUGH_MEMORY, off=0x%llx len=%llu)",
             ceiling, (unsigned long long)offset,
             (unsigned long long)req_len);
    trt_core_log(line);
    trt_SetLastError(8 /*ERROR_NOT_ENOUGH_MEMORY*/);
    return nullptr;
  }

  // LAYER 1 — map, don't copy. A read-only view over a real file needs no
  // private copy: the pages already live in the page cache, and mapping them
  // costs address space, not commit. Under pressure the kernel drops the clean
  // pages and the next touch re-faults them from the SSD, so a streaming game's
  // multi-GB asset archive behaves as if it fit inside the 5 GB budget.
  //
  // Only a view the guest can solely READ may be mapped: FILE_MAP_COPY (0x01)
  // is writable through private COW pages, FILE_MAP_WRITE (0x02) needs real
  // write-back, and FILE_MAP_EXECUTE (0x20) needs executable protection.
  // Handing any of them a PROT_READ mapping would fault the guest on its first
  // store (or instruction fetch), so they keep the private-copy path.
  const bool read_only_view = (access & (0x01u | 0x02u | 0x20u)) == 0;
  const bool flush_back =
      ((access & 0x02u) != 0 || access == 0xF001Fu) && (access != 0x01u);
  bool file_backed = false;
  void* view_ptr = nullptr;
  void* map_base = nullptr;
  size_t map_span = 0;
  u64 charge_span = 0;
  if (read_only_view && !flush_back && core->file_handle &&
      file_view_mode_enabled()) {
#ifdef __linux__
    u64 fsz = 0;
    const int fd = trt_file_host_fd(core->file_handle, &fsz);
    // Only a view that lies entirely inside the file can be mapped. A view
    // reaching past EOF (CreateFileMapping with max_size > file size) must
    // copy, so the tail stays zero-filled instead of faulting on a page the
    // file does not have.
    if (fd >= 0 && fsz > 0 && offset < fsz && req_len <= fsz - offset) {
      const size_t page = (size_t)sysconf(_SC_PAGESIZE);
      const u64 base_off = offset & ~(u64)(page - 1);
      const size_t delta = (size_t)(offset - base_off);
      const size_t span = delta + (size_t)req_len;
      if (trt_mem_budget_charge_file_view((u64)span) == 0) {
        void* m = mmap(nullptr, span, PROT_READ, MAP_PRIVATE, fd,
                       (off_t)base_off);
        if (m != MAP_FAILED) {
          // async SSD -> page-cache readahead; the fault path stays lazy
          if (file_view_readahead_enabled()) {
            (void)madvise(m, span, MADV_WILLNEED);
          }
          view_ptr = (u8*)m + delta;
          file_backed = true;
          map_base = m;
          map_span = span;
          charge_span = (u64)span;
          log_file_view_once();
        } else {
          trt_mem_budget_discharge_file_view((u64)span);
        }
      }
    }
#endif
  }

  bool from_be = false;
  if (!file_backed) {
    // Budget: the view's bytes join the same ledger as VirtualAlloc commit.
    // Refusal fails the map honestly (named, 1455) — the guest's streaming
    // layer retries with a smaller window instead of walking off the 5 GB
    // cliff and getting the whole process killed by the platform.
    if (trt_mem_budget_charge_map((u64)req_len) != 0) return nullptr;
    view_ptr = alloc_view_buf((size_t)req_len, &from_be);
    if (!view_ptr) {
      trt_mem_budget_discharge_map((u64)req_len);
      trt_SetLastError(8 /*ERROR_NOT_ENOUGH_MEMORY*/);
      return nullptr;
    }

    // The backend may hand back the SAME address an evicted view used to have
    // (the pages were released). Any evicted-range record overlapping this
    // live view is stale from this instant — purge it, or every later
    // unmap/flush of this live view would be falsely named use-after-evict and
    // refused.
    for (size_t i = 0; i < g_evicted.size();) {
      const uintptr_t s = g_evicted[i].ptr;
      const uintptr_t e = s + g_evicted[i].bytes;
      const uintptr_t vs = (uintptr_t)view_ptr;
      const uintptr_t ve = vs + (size_t)req_len;
      if (s < ve && vs < e) {
        g_evicted.erase(g_evicted.begin() + (ptrdiff_t)i);
      } else {
        ++i;
      }
    }

    if (core->file_handle) {
      size_t got = 0;
      if (!trt_file_pread(core->file_handle, offset, view_ptr,
                          (size_t)req_len, &got)) {
        trt_mem_budget_discharge_map((u64)req_len);
        free_view_buf(view_ptr, (size_t)req_len, from_be);
        trt_SetLastError(k_err_invalid_handle);
        return nullptr;
      }
      if (got < (size_t)req_len) {
        memset((u8*)view_ptr + got, 0, (size_t)req_len - got);
      }
    } else {
      // Lazily allocate shared pagefile backing for reasonable anonymous
      // mappings
      if (!core->anon_buf && core->size <= (128ull << 20)) {
        // the backing buffer is real committed memory: charge it (commit +
        // VA); on refusal leave anon_buf null — the view still works, it
        // just reads zeros instead of shared backing (existing graceful path)
        const u64 sz = core->size;
        bool charged = false;
        if (trt_mem_budget_charge_va(sz) == 0) {
          if (trt_mem_budget_charge_commit(sz) == 0) {
            charged = true;
          } else {
            trt_mem_budget_discharge_va(sz);  // commit refused: roll VA back
          }
        }
        if (charged) {
          core->anon_buf = (u8*)calloc(1, (size_t)sz);
          if (core->anon_buf) {
            core->anon_cap = sz;
          } else {
            trt_mem_budget_discharge_commit(sz);
            trt_mem_budget_discharge_va(sz);
          }
        }
      }
      if (core->anon_buf && offset < core->anon_cap) {
        size_t cp = (size_t)req_len;
        if (offset + cp > core->anon_cap) {
          cp = (size_t)(core->anon_cap - offset);
        }
        memcpy(view_ptr, core->anon_buf + offset, cp);
      }
    }
  }  // if (!file_backed)

  // A file-backed mapping is already PROT_READ; only the copy path needs a
  // host re-protect.
  if (!file_backed) {
    const int prot_bits = map_access_to_prot_bits(access, core->protect);
    const trt_backend* be = trt_core_backend();
    if (be && be->protect && prot_bits != (PROT_READ_ | PROT_WRITE_)) {
      be->protect(view_ptr, (size_t)req_len, prot_bits_to_trt(prot_bits));
    }
  }

  mapped_view mv{};
  mv.ptr = view_ptr;
  mv.bytes = (size_t)req_len;
  mv.offset = offset;
  mv.core = core;
  mv.flush_on_unmap = flush_back;
  mv.from_be = from_be;
  mv.stamp = ++g_map_clock;  // LRU touch: this map is the most recent use
  mv.file_backed = file_backed;
  mv.map_base = map_base;
  mv.map_span = map_span;
  mv.charge_span = charge_span;
  ++core->view_refs;
  g_views.push_back(mv);

  char line[192];
  snprintf(line, sizeof(line),
           "trt: MapViewOfFile(h=%p off=0x%llx len=%llu) -> %p",
           (void*)(uintptr_t)h, (unsigned long long)offset,
           (unsigned long long)req_len, view_ptr);
  trt_core_log(line);
  trt_SetLastError(0);
  return view_ptr;
}

void* TRT_STDCALL trt_MapViewOfFile(u64 h, u32 access, u32 off_high,
                                    u32 off_low, u64 bytes) {
  return trt_MapViewOfFileEx(h, access, off_high, off_low, bytes, nullptr);
}

int TRT_STDCALL trt_FlushViewOfFile(void* view, u64 bytes) {
  if (!view) return 0;
  std::lock_guard<std::mutex> lk(g_mem_mu);
  if (hit_evicted_locked(view, "flush")) return 0;  // view was evicted: gone
  const uintptr_t va = (uintptr_t)view;
  for (mapped_view& mv : g_views) {
    const uintptr_t base = (uintptr_t)mv.ptr;
    if (va >= base && va < base + mv.bytes) {
      mv.stamp = ++g_map_clock;  // LRU touch: a flush is active use
      const size_t sub = (size_t)(va - base);
      size_t len = bytes ? (size_t)bytes : (mv.bytes - sub);
      if (sub + len > mv.bytes) len = mv.bytes - sub;
      return flush_view_locked(mv, sub, len) ? 1 : 0;
    }
  }
  return 0;
}

int TRT_STDCALL trt_UnmapViewOfFile(void* view) {
  if (!view) return 0;
  std::lock_guard<std::mutex> lk(g_mem_mu);
  if (hit_evicted_locked(view, "unmap")) return 0;  // view was evicted: gone
  for (size_t i = 0; i < g_views.size(); ++i) {
    if (g_views[i].ptr == view) {
      const mapped_view mv = g_views[i];
      g_views.erase(g_views.begin() + (ptrdiff_t)i);
      if (mv.flush_on_unmap) flush_view_locked(mv, 0, mv.bytes);
      release_view_charge_locked(mv);
      release_view_backing_locked(mv);
      if (mv.core && mv.core->view_refs > 0) --mv.core->view_refs;
      maybe_destroy_core_locked(mv.core);
      return 1;
    }
  }
  return 0;
}

static void win32_prot_to_trt(u32 page, unsigned* prot, u32* old_code) {
  switch (page & 0xff) {
    case 0x01: *prot = 0; *old_code = 0x01; break;
    case 0x02: *prot = TRT_PROT_R; *old_code = 0x02; break;
    case 0x04: *prot = TRT_PROT_R | TRT_PROT_W; *old_code = 0x04; break;
    case 0x08: *prot = TRT_PROT_R | TRT_PROT_W; *old_code = 0x08; break;
    case 0x10: *prot = TRT_PROT_X; *old_code = 0x10; break;
    case 0x20: *prot = TRT_PROT_R | TRT_PROT_X; *old_code = 0x20; break;
    case 0x40: *prot = TRT_PROT_R | TRT_PROT_W | TRT_PROT_X; *old_code = 0x40; break;
    case 0x80: *prot = TRT_PROT_R | TRT_PROT_W | TRT_PROT_X; *old_code = 0x80; break;
    default: *prot = 0; *old_code = 0x01; break;
  }
}

int TRT_STDCALL trt_VirtualProtect(void* addr, u64 size, u32 new_protect,
                                   u32* old_protect) {
  const trt_backend* be = trt_core_backend();
  if (!be || !be->protect) return 0;
  if (!addr || size == 0) {
    trt_SetLastError(k_err_invalid_parameter);
    return 0;
  }
  const uintptr_t raw = (uintptr_t)addr;
  const uintptr_t page = raw & ~(uintptr_t)4095ULL;
  const size_t span = (size_t)((raw - page) + (size_t)size);
  const size_t aligned = (span + 4095ULL) & ~(size_t)4095ULL;
  unsigned prot = 0;
  u32 old_code = 0x04;
  win32_prot_to_trt(new_protect, &prot, &old_code);
  if (new_protect == 0x04u && size <= SIZE_MAX &&
      trt_va_is_deferred_readwrite(addr, static_cast<size_t>(size))) {
    if (old_protect) *old_protect = 0x04u;
    return 1;
  }
  if (!trt_va_prepare_data_range(addr, static_cast<size_t>(size))) return 0;
  const int rc = be->protect((void*)page, aligned, prot);
  if (rc != 0) {
    // W^X two-phase JIT fallback: when host denies RWX (0x40/0x80), arm as RW
    // and promote to RX on FlushInstructionCache or first NX fault.
    if ((new_protect & 0xFFu) == 0x40u || (new_protect & 0xFFu) == 0x80u) {
      if (be->protect((void*)page, aligned, TRT_PROT_R | TRT_PROT_W) == 0) {
        u32 prev = old_code;
        if (trt_va_mark_pending_rx((void*)page, aligned, &prev)) {
          trt_core_log("kernel32: VirtualProtect(RWX) -> W^X two-phase JIT (armed RW, flips RX on FlushInstructionCache)");
          if (old_protect) *old_protect = prev;
          return 1;
        }
      }
    }
    if (new_protect & 0xF0u) {
      trt_core_log("kernel32: VirtualProtect(EXEC) rejected by host backend");
    }
    trt_SetLastError(87);
    return 0;
  }
  u32 prev = old_code;
  trt_va_notify_protect((void*)page, aligned, new_protect & 0xFFu, &prev);
  if (old_protect) *old_protect = prev;
  return 1;
}

static bool is_current_proc_handle(void* proc) {
  const uintptr_t v = (uintptr_t)proc;
  return proc == nullptr || v == (uintptr_t)(intptr_t)-1 || v == 0xFFFFFFFFULL;
}

int TRT_STDCALL trt_VirtualProtectEx(void* proc, void* addr, u64 size,
                                     u32 new_protect, u32* old_protect) {
  if (!is_current_proc_handle(proc)) {
    trt_SetLastError(5);  // ERROR_ACCESS_DENIED
    return 0;
  }
  return trt_VirtualProtect(addr, size, new_protect, old_protect);
}

extern "C" void* TRT_STDCALL trt_VirtualAlloc(void* addr, u64 size, u32 type, u32 prot);
extern "C" int TRT_STDCALL trt_VirtualFree(void* addr, u64 bytes, u32 type);
extern "C" u64 TRT_STDCALL trt_VirtualQuery(const void* addr, void* mbi_out, u64 len);

void* TRT_STDCALL trt_VirtualAllocEx(void* proc, void* addr, u64 size,
                                     u32 type, u32 prot) {
  if (!is_current_proc_handle(proc)) {
    trt_SetLastError(5);
    return nullptr;
  }
  return trt_VirtualAlloc(addr, size, type, prot);
}

void* TRT_STDCALL trt_VirtualAlloc2(void* proc, void* base_addr, u64 size,
                                    u32 type, u32 prot, void* ext_params,
                                    u32 param_count) {
  (void)ext_params;
  (void)param_count;
  return trt_VirtualAllocEx(proc, base_addr, size, type, prot);
}

void* TRT_STDCALL trt_VirtualAlloc2FromApp(void* proc, void* base_addr, u64 size,
                                           u32 type, u32 prot, void* ext_params,
                                           u32 param_count) {
  (void)ext_params;
  (void)param_count;
  return trt_VirtualAllocEx(proc, base_addr, size, type, prot);
}

int TRT_STDCALL trt_VirtualFreeEx(void* proc, void* addr, u64 bytes, u32 type) {
  if (!is_current_proc_handle(proc)) {
    trt_SetLastError(5);
    return 0;
  }
  return trt_VirtualFree(addr, bytes, type);
}

u64 TRT_STDCALL trt_VirtualQueryEx(void* proc, const void* addr, void* mbi_out,
                                   u64 len) {
  if (!is_current_proc_handle(proc)) {
    trt_SetLastError(5);
    return 0;
  }
  return trt_VirtualQuery(addr, mbi_out, len);
}

int trt_bridge_register_memory(void) {
  static const trt_named_fn k_fns[] = {
      {"CreateFileMappingW", (void*)&trt_CreateFileMappingW},
      {"CreateFileMappingA", (void*)&trt_CreateFileMappingA},
      {"CreateFileMappingNumaW", (void*)&trt_CreateFileMappingNumaW},
      {"MapViewOfFile", (void*)&trt_MapViewOfFile},
      {"MapViewOfFileEx", (void*)&trt_MapViewOfFileEx},
      {"UnmapViewOfFile", (void*)&trt_UnmapViewOfFile},
      {"FlushViewOfFile", (void*)&trt_FlushViewOfFile},
      {"OpenFileMappingW", (void*)&trt_OpenFileMappingW},
      {"OpenFileMappingA", (void*)&trt_OpenFileMappingA},
      {"VirtualProtect", (void*)&trt_VirtualProtect},
      {"VirtualProtectEx", (void*)&trt_VirtualProtectEx},
      {"VirtualAllocEx", (void*)&trt_VirtualAllocEx},
      {"VirtualAlloc2", (void*)&trt_VirtualAlloc2},
      {"VirtualAlloc2FromApp", (void*)&trt_VirtualAlloc2FromApp},
      {"VirtualFreeEx", (void*)&trt_VirtualFreeEx},
      {"VirtualQueryEx", (void*)&trt_VirtualQueryEx},
  };
  trt_module_register("kernel32.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  trt_module_register("kernelbase.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  return 0;
}

}  // extern "C"
