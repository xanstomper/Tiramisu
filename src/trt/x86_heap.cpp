// tiramisu/src/trt/x86_heap.cpp
// Guest-arena allocator for 32-bit guests.
//
// Why this exists: a 32-bit Windows program allocates with HeapAlloc /
// VirtualAlloc / malloc and stores the RESULT in a 32-bit variable. Handing it
// a host malloc() pointer truncates the address to garbage, and the next
// bridge call that receives it gets translated through the committed-page
// heuristic and sees an enormous bogus size ("VA reservation of 136252576..."
// in the OpenTTD boot log). So every allocation the guest can make must live
// INSIDE the 4 GB reservation, where a guest VA is a valid host pointer.
//
// This is a first-fit allocator over [0x20000000, 0xC0000000):
//   - 8-byte block header {size, magic}; size includes the header
//   - freed blocks are chained through the first word of their payload
//   - pages are committed on demand through the existing x86_mem bitmap
//   - no coalescing: a free block is reused only when it is big enough, so a
//     long-running guest can fragment. That is a named limitation, not a
//     silent one, and x86_guest_heap_stats() reports it.
#include "x86_cpu.h"

#include <cstdio>
#include <cstring>

namespace {

constexpr uint32_t kArenaBase = 0x20000000u;
// Loader-owned image ranges are skipped below.
constexpr uint32_t kArenaEnd = 0xC0000000u;
constexpr uint32_t kMagicUsed = 0x48454150u;  // 'HEAP'
constexpr uint32_t kMagicFree = 0x46524545u;  // 'FREE'
constexpr uint32_t kHdrSize = 8u;
constexpr uint32_t kMinSplit = 32u;  // don't split off a sliver

struct blk_hdr {
  uint32_t size;   // total block size including this header
  uint32_t magic;
};

struct heap_state {
  uint32_t bump = kArenaBase;
  uint32_t free_head = 0;   // guest VA of the first free block (0 = none)
  uint64_t allocs = 0;
  uint64_t frees = 0;
  uint64_t live_bytes = 0;
  uint64_t peak_bytes = 0;
};

heap_state g_heap;

inline blk_hdr* hdr_at(x86_context* ctx, uint32_t va) {
  return reinterpret_cast<blk_hdr*>(ctx->mem.base + va);
}

inline uint32_t align8(uint32_t v) { return (v + 7u) & ~7u; }

// Reject a pointer that cannot be one of ours before touching it.
bool valid_block(x86_context* ctx, uint32_t payload) {
  if (!ctx->mem.base) return false;
  if (payload < kArenaBase + kHdrSize || payload >= kArenaEnd) return false;
  const uint32_t hdr_va = payload - kHdrSize;
  if (!x86_page_mapped(ctx, hdr_va >> X86_PAGE_SHIFT)) return false;
  const blk_hdr* h = hdr_at(ctx, hdr_va);
  if (h->magic != kMagicUsed) return false;
  if (h->size < kHdrSize + 8u) return false;
  if (hdr_va + h->size > kArenaEnd) return false;
  return true;
}

void unlink_free(x86_context* ctx, uint32_t hdr_va) {
  uint32_t* payload = reinterpret_cast<uint32_t*>(ctx->mem.base + hdr_va + kHdrSize);
  const uint32_t next = payload[0];
  const uint32_t prev = payload[1];
  if (prev) {
    uint32_t* pprev = reinterpret_cast<uint32_t*>(ctx->mem.base + prev + kHdrSize);
    pprev[0] = next;
  } else {
    g_heap.free_head = next;
  }
  if (next) {
    uint32_t* pnext = reinterpret_cast<uint32_t*>(ctx->mem.base + next + kHdrSize);
    pnext[1] = prev;
  }
}

void push_free(x86_context* ctx, uint32_t hdr_va, uint32_t size) {
  blk_hdr* h = hdr_at(ctx, hdr_va);
  h->size = size;
  h->magic = kMagicFree;
  uint32_t* payload = reinterpret_cast<uint32_t*>(ctx->mem.base + hdr_va + kHdrSize);
  payload[0] = g_heap.free_head;  // next
  payload[1] = 0;                 // prev
  if (g_heap.free_head) {
    uint32_t* head = reinterpret_cast<uint32_t*>(
        ctx->mem.base + g_heap.free_head + kHdrSize);
    head[1] = hdr_va;
  }
  g_heap.free_head = hdr_va;
}

// Carve `need` bytes (header included) out of the arena. Returns a header VA.
uint32_t carve(x86_context* ctx, uint32_t need) {
  // First fit over the free list.
  uint32_t cur = g_heap.free_head;
  while (cur) {
    blk_hdr* h = hdr_at(ctx, cur);
    if (h->size >= need) {
      unlink_free(ctx, cur);
      if (h->size - need >= kMinSplit) {
        push_free(ctx, cur + need, h->size - need);
        h->size = need;
      }
      h->magic = kMagicUsed;
      return cur;
    }
    uint32_t* payload = reinterpret_cast<uint32_t*>(ctx->mem.base + cur + kHdrSize);
    cur = payload[0];
  }

  uint32_t va = g_heap.bump;
  while ((uint64_t)va + need <= kArenaEnd) {
    const uint32_t image_end = trt_pe32_image_overlap_end(va, need);
    if (!image_end) break;
    va = align8(image_end);
  }
  if ((uint64_t)va + need > kArenaEnd || x86_mem_commit(ctx, va, need) != 0)
    return 0;
  g_heap.bump = va + need;
  blk_hdr* h = hdr_at(ctx, va);
  h->size = need;
  h->magic = kMagicUsed;
  return va;
}

}  // namespace

// ---- public ---------------------------------------------------------------

// Struct declared in x86_cpu.h; repeated here so this file also builds
// against an older header (the corpus harness compiles files directly).
struct x86_heap_stats;

void x86_guest_heap_reset(void) {
  g_heap = heap_state();
}

uint32_t x86_guest_alloc(x86_context* ctx, uint32_t bytes, int zero) {
  if (!ctx || !ctx->mem.base) return 0;
  if (bytes == 0) bytes = 1;
  const uint32_t need = align8(bytes) + kHdrSize;
  if (need < bytes) return 0;  // overflow
  const uint32_t hdr_va = carve(ctx, need);
  if (!hdr_va) {
    char line[128];
    snprintf(line, sizeof(line),
             "x86_heap: OUT OF ARENA for %u bytes (guest heap exhausted)",
             (unsigned)bytes);
    if (ctx->log) ctx->log(line);
    return 0;
  }
  const uint32_t payload = hdr_va + kHdrSize;
  if (zero) memset(ctx->mem.base + payload, 0, bytes);
  g_heap.allocs++;
  g_heap.live_bytes += hdr_at(ctx, hdr_va)->size;
  if (g_heap.live_bytes > g_heap.peak_bytes) g_heap.peak_bytes = g_heap.live_bytes;
  return payload;
}

int x86_guest_free(x86_context* ctx, uint32_t ptr) {
  if (!ctx || !ptr) return 0;
  if (!valid_block(ctx, ptr)) {
    char line[160];
    snprintf(line, sizeof(line),
             "x86_heap: free of 0x%08X refused (not a live arena block)",
             (unsigned)ptr);
    if (ctx->log) ctx->log(line);
    return 0;
  }
  const uint32_t hdr_va = ptr - kHdrSize;
  const uint32_t size = hdr_at(ctx, hdr_va)->size;
  g_heap.live_bytes -= (g_heap.live_bytes >= size) ? size : g_heap.live_bytes;
  g_heap.frees++;
  push_free(ctx, hdr_va, size);
  return 1;
}

uint32_t x86_guest_realloc(x86_context* ctx, uint32_t ptr, uint32_t bytes,
                           int zero) {
  if (!ctx || !ctx->mem.base) return 0;
  if (!ptr) return x86_guest_alloc(ctx, bytes, zero);
  if (!valid_block(ctx, ptr)) return x86_guest_alloc(ctx, bytes, zero);
  const uint32_t hdr_va = ptr - kHdrSize;
  const uint32_t have = hdr_at(ctx, hdr_va)->size - kHdrSize;
  if (bytes <= have) return ptr;  // in place
  const uint32_t fresh = x86_guest_alloc(ctx, bytes, zero);
  if (!fresh) return 0;
  memcpy(ctx->mem.base + fresh, ctx->mem.base + ptr, have);
  if (zero) memset(ctx->mem.base + fresh + have, 0, bytes - have);
  x86_guest_free(ctx, ptr);
  return fresh;
}

uint32_t x86_guest_blocksize(x86_context* ctx, uint32_t ptr) {
  if (!ctx || !valid_block(ctx, ptr)) return 0;
  return hdr_at(ctx, ptr - kHdrSize)->size - kHdrSize;
}

int x86_guest_ptr_in_arena(uint32_t ptr) {
  return ptr >= kArenaBase && ptr < kArenaEnd;
}

void x86_guest_heap_stats(x86_heap_stats* out) {
  if (!out) return;
  out->arena_base = kArenaBase;
  out->arena_end = kArenaEnd;
  out->bump = g_heap.bump;
  out->allocs = g_heap.allocs;
  out->frees = g_heap.frees;
  out->live_bytes = g_heap.live_bytes;
  out->peak_bytes = g_heap.peak_bytes;
}
