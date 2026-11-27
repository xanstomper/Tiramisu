// tiramisu/src/trt/bridge_dstorage.cpp
// DirectStorage (dstorage.dll / dstoragecore.dll) bridge.
//
// Implements IDStorageFactory, IDStorageFile, IDStorageQueue/1/2, and
// IDStorageStatusArray over Tiramisu's VFS, async worker thread execution,
// Win32 Event signaling, D3D12 fence signaling, and D3D12 buffer Map/Unmap.
#include "bridge_dstorage.h"
#include "thunk.h"
#include "trt.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <sys/stat.h>
#else
#include <sys/stat.h>
#endif

extern "C" {
u64 TRT_STDCALL trt_CreateEventW(void* attrs, bool manual, bool initial,
                                 const u16* name);
int TRT_STDCALL trt_SetEvent(u64 handle);
int TRT_STDCALL trt_CloseHandle(void* handle);
}

namespace {

static constexpr long k_s_ok = 0L;
static constexpr long k_e_fail = (long)0x80004005L;
static constexpr long k_e_notimpl = (long)0x80004001L;
static constexpr long k_e_nointerface = (long)0x80004002L;
static constexpr long k_e_pointer = (long)0x80004003L;
static constexpr long k_e_invalidarg = (long)0x80070057L;
static constexpr long k_e_pending = (long)0x8000000AL;
static constexpr long k_e_file_not_found = (long)0x80070002L;
static constexpr long k_e_outofmemory = (long)0x8007000EL;
static constexpr long k_e_dstorage_invalid_queue_cap = (long)0x89240008L;

struct ds_guid {
  u32 d1;
  u16 d2;
  u16 d3;
  u8 d4[8];
};

static bool guid_eq(const ds_guid* a, const ds_guid& b) {
  if (!a) return false;
  return a->d1 == b.d1 && a->d2 == b.d2 && a->d3 == b.d3 &&
         memcmp(a->d4, b.d4, 8) == 0;
}

static constexpr ds_guid k_iid_unknown = {
    0x00000000u, 0x0000u, 0x0000u, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static constexpr ds_guid k_iid_factory = {
    0x6924ea0cu, 0xc3cdu, 0x4826u, {0xb1, 0x0a, 0xf6, 0x4f, 0x4e, 0xd9, 0x27, 0xc1}};
static constexpr ds_guid k_iid_file = {
    0x5de95e7bu, 0x955au, 0x4868u, {0xa7, 0x3c, 0x24, 0x3b, 0x29, 0xf4, 0xb8, 0xda}};
static constexpr ds_guid k_iid_status_array = {
    0x82397587u, 0x7cd5u, 0x453bu, {0xa0, 0x2e, 0x31, 0x37, 0x9b, 0xd6, 0x46, 0x56}};
static constexpr ds_guid k_iid_queue = {
    0xcfdbd83fu, 0x9e06u, 0x4fdau, {0x8e, 0xa5, 0x69, 0x04, 0x21, 0x37, 0xf4, 0x9b}};
static constexpr ds_guid k_iid_queue1 = {
    0xdd2f482cu, 0x5effu, 0x41e8u, {0x9c, 0x9e, 0xd2, 0x37, 0x4b, 0x27, 0x81, 0x28}};
static constexpr ds_guid k_iid_queue2 = {
    0xb1c9d643u, 0x3a49u, 0x44a2u, {0xb4, 0x6f, 0x65, 0x36, 0x49, 0x47, 0x0d, 0x18}};

// Explicit fixed-offset mirrors matching Microsoft.Direct3D.DirectStorage 1.2.3
// Win64 ABI regardless of host compiler bitfield packing rules.
#pragma pack(push, 8)
struct ds_queue_desc {
  u64 source_type;
  u16 capacity;
  int8_t priority;
  const char* name;
  void* device;
};

struct ds_queue_info {
  ds_queue_desc desc;
  u16 empty_slot_count;
  u16 requests_until_auto_submit;
};

struct ds_by_handle_file_info {
  u32 file_attributes;
  u32 creation_time[2];
  u32 last_access_time[2];
  u32 last_write_time[2];
  u32 volume_serial_number;
  u32 file_size_high;
  u32 file_size_low;
  u32 number_of_links;
  u32 file_index_high;
  u32 file_index_low;
};

struct ds_raw_request {
  u8 options[16];     // +0: CompressionFormat (1B), Reserved1 (7B), flags (8B)
  u8 source[24];      // +16: DSTORAGE_SOURCE
  u8 destination[40]; // +40: DSTORAGE_DESTINATION
  u32 uncompressed_size;
  u64 cancellation_tag;
  const char* name;
};
#pragma pack(pop)

static_assert(sizeof(ds_by_handle_file_info) == 52, "Win32 BY_HANDLE_FILE_INFORMATION size");
static_assert(sizeof(ds_raw_request) == 104, "Win64 DSTORAGE_REQUEST size");

static u8 req_compression(const ds_raw_request& r) { return r.options[0]; }
static u8 req_source_type(const ds_raw_request& r) {
  u64 flags = 0;
  memcpy(&flags, r.options + 8, 8);
  return (u8)(flags & 1u);
}
static u8 req_dest_type(const ds_raw_request& r) {
  u64 flags = 0;
  memcpy(&flags, r.options + 8, 8);
  return (u8)((flags >> 1) & 0x7Fu);
}

static std::string utf16_to_utf8(const u16* s) {
  std::string out;
  if (!s) return out;
  for (size_t i = 0; s[i]; ++i) {
    u32 cp = s[i];
    if (cp >= 0xD800u && cp <= 0xDBFFu && s[i + 1] >= 0xDC00u && s[i + 1] <= 0xDFFFu) {
      cp = 0x10000u + (((cp - 0xD800u) << 10) | (s[i + 1] - 0xDC00u));
      ++i;
    }
    if (cp < 0x80u) {
      out.push_back((char)cp);
    } else if (cp < 0x800u) {
      out.push_back((char)(0xC0u | (cp >> 6)));
      out.push_back((char)(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000u) {
      out.push_back((char)(0xE0u | (cp >> 12)));
      out.push_back((char)(0x80u | ((cp >> 6) & 0x3Fu)));
      out.push_back((char)(0x80u | (cp & 0x3Fu)));
    } else {
      out.push_back((char)(0xF0u | (cp >> 18)));
      out.push_back((char)(0x80u | ((cp >> 12) & 0x3Fu)));
      out.push_back((char)(0x80u | ((cp >> 6) & 0x3Fu)));
      out.push_back((char)(0x80u | (cp & 0x3Fu)));
    }
  }
  return out;
}

static std::string resolve_dstorage_path(const u16* wpath) {
  char resolved[1024] = {};
  if (wpath && trt_vfs_resolve_utf16(wpath, resolved, sizeof(resolved)) == 0 && resolved[0])
    return std::string(resolved);
  std::string raw = utf16_to_utf8(wpath);
#if !defined(_WIN32)
  for (char& c : raw)
    if (c == '\\') c = '/';
#endif
  return raw;
}

struct ds_file {
  const void* const* vtbl;
  std::atomic<u32> ref_count{1};
  std::mutex mu;
  std::string host_path;
  u64 file_size = 0;
  bool closed = false;
};

struct ds_status_array {
  const void* const* vtbl;
  std::atomic<u32> ref_count{1};
  std::mutex mu;
  std::vector<long> slots;
  std::string name;
};

struct ds_queue_cmd {
  enum kind_t { cmd_request, cmd_status, cmd_signal, cmd_event } kind = cmd_request;
  ds_raw_request req{};
  ds_file* retained_file = nullptr;
  ds_status_array* status_arr = nullptr;
  u32 status_idx = 0;
  void* fence = nullptr;
  u64 fence_value = 0;
  u64 event_handle = 0;
};

struct ds_queue {
  const void* const* vtbl;
  std::atomic<u32> ref_count{1};
  ds_queue_desc desc{};
  std::string name_copy;
  std::mutex mu;
  std::condition_variable cv;
  std::deque<ds_queue_cmd> pending;
  u64 error_event = 0;
  u32 failure_count = 0;
  long first_failure_hr = k_s_ok;
  i32 first_failure_cmd = -1;
  bool closed = false;
  bool stop_worker = false;
  std::thread worker;
  long batch_hr = k_s_ok;
};

struct ds_factory {
  const void* const* vtbl;
  std::atomic<u32> ref_count{1};
  u32 staging_size = 32u * 1048576u;
  u32 debug_flags = 0;
};

typedef void(TRT_GUEST_ABI* com_release_fn)(void*);
typedef void(TRT_GUEST_ABI* com_addref_fn)(void*);
typedef long(TRT_GUEST_ABI* fence_signal_fn)(void*, u64);
typedef long(TRT_GUEST_ABI* res_map_fn)(void*, u32, const void*, void**);
typedef void(TRT_GUEST_ABI* res_unmap_fn)(void*, u32, const void*);

static void com_addref_any(void* obj) {
  if (!obj) return;
  auto** vt = *(void***)obj;
  ((com_addref_fn)vt[1])(obj);
}

static void com_release_any(void* obj) {
  if (!obj) return;
  auto** vt = *(void***)obj;
  ((com_release_fn)vt[2])(obj);
}

// ---- IDStorageFile ----------------------------------------------------------

static long TRT_GUEST_ABI file_qi(void* self, const ds_guid* riid, void** ppv) {
  if (!ppv) return k_e_pointer;
  *ppv = nullptr;
  if (!riid) return k_e_invalidarg;
  if (guid_eq(riid, k_iid_unknown) || guid_eq(riid, k_iid_file)) {
    *ppv = self;
    com_addref_any(self);
    return k_s_ok;
  }
  return k_e_nointerface;
}

static u32 TRT_GUEST_ABI file_addref(void* self) {
  return ++((ds_file*)self)->ref_count;
}

static u32 TRT_GUEST_ABI file_release(void* self) {
  auto* f = (ds_file*)self;
  const u32 n = --f->ref_count;
  if (n == 0) delete f;
  return n;
}

static void TRT_GUEST_ABI file_close(void* self) {
  auto* f = (ds_file*)self;
  std::lock_guard<std::mutex> lk(f->mu);
  f->closed = true;
}

static long TRT_GUEST_ABI file_get_info(void* self, ds_by_handle_file_info* info) {
  if (!info) return k_e_pointer;
  memset(info, 0, sizeof(*info));
  auto* f = (ds_file*)self;
  std::lock_guard<std::mutex> lk(f->mu);
  if (f->closed) return k_e_fail;
  info->file_attributes = 0x80u;  // FILE_ATTRIBUTE_NORMAL
  info->file_size_low = (u32)(f->file_size & 0xFFFFFFFFu);
  info->file_size_high = (u32)((f->file_size >> 32) & 0xFFFFFFFFu);
  info->number_of_links = 1;
  return k_s_ok;
}

static const void* const k_file_vtbl[] = {
    (const void*)&file_qi,
    (const void*)&file_addref,
    (const void*)&file_release,
    (const void*)&file_close,
    (const void*)&file_get_info,
};

// ---- IDStorageStatusArray ---------------------------------------------------

static long TRT_GUEST_ABI status_qi(void* self, const ds_guid* riid, void** ppv) {
  if (!ppv) return k_e_pointer;
  *ppv = nullptr;
  if (!riid) return k_e_invalidarg;
  if (guid_eq(riid, k_iid_unknown) || guid_eq(riid, k_iid_status_array)) {
    *ppv = self;
    com_addref_any(self);
    return k_s_ok;
  }
  return k_e_nointerface;
}

static u32 TRT_GUEST_ABI status_addref(void* self) {
  return ++((ds_status_array*)self)->ref_count;
}

static u32 TRT_GUEST_ABI status_release(void* self) {
  auto* s = (ds_status_array*)self;
  const u32 n = --s->ref_count;
  if (n == 0) delete s;
  return n;
}

static u8 TRT_GUEST_ABI status_is_complete(void* self, u32 index) {
  auto* s = (ds_status_array*)self;
  std::lock_guard<std::mutex> lk(s->mu);
  if (index >= s->slots.size()) return 1;
  return s->slots[index] != k_e_pending ? 1 : 0;
}

static long TRT_GUEST_ABI status_get_hr(void* self, u32 index) {
  auto* s = (ds_status_array*)self;
  std::lock_guard<std::mutex> lk(s->mu);
  if (index >= s->slots.size()) return k_e_invalidarg;
  return s->slots[index];
}

static const void* const k_status_vtbl[] = {
    (const void*)&status_qi,
    (const void*)&status_addref,
    (const void*)&status_release,
    (const void*)&status_is_complete,
    (const void*)&status_get_hr,
};

// ---- IDStorageQueue / 1 / 2 -------------------------------------------------

static long execute_request(const ds_queue_cmd& cmd) {
  const ds_raw_request& r = cmd.req;
  const u8 comp = req_compression(r);
  if (comp != 0) return k_e_notimpl;  // compression codecs not wired yet

  const u8 src_type = req_source_type(r);
  const u8 dst_type = req_dest_type(r);

  // Extract source payload location + size.
  u64 file_off = 0;
  const void* mem_src = nullptr;
  u32 src_size = 0;
  ds_file* file_obj = nullptr;
  if (src_type == 0) {
    memcpy(&file_obj, r.source + 0, 8);
    memcpy(&file_off, r.source + 8, 8);
    memcpy(&src_size, r.source + 16, 4);
    if (!file_obj) return k_e_invalidarg;
  } else {
    memcpy(&mem_src, r.source + 0, 8);
    memcpy(&src_size, r.source + 8, 4);
    if (!mem_src && src_size > 0) return k_e_invalidarg;
  }

  // Resolve destination CPU-accessible pointer.
  u8* dst_ptr = nullptr;
  u32 dst_size = 0;
  void* mapped_res = nullptr;
  u64 buf_off = 0;
  if (dst_type == 0) {
    memcpy(&dst_ptr, r.destination + 0, 8);
    memcpy(&dst_size, r.destination + 8, 4);
    if (!dst_ptr && dst_size > 0) return k_e_invalidarg;
  } else if (dst_type == 1) {
    memcpy(&mapped_res, r.destination + 0, 8);
    memcpy(&buf_off, r.destination + 8, 8);
    memcpy(&dst_size, r.destination + 16, 4);
    if (!mapped_res) return k_e_invalidarg;
    void* base = nullptr;
    auto** vt = *(void***)mapped_res;
    const long mhr = ((res_map_fn)vt[8])(mapped_res, 0, nullptr, &base);
    if (mhr < 0 || !base) return mhr < 0 ? mhr : k_e_fail;
    dst_ptr = (u8*)base + buf_off;
  } else {
    return k_e_notimpl;
  }

  const u32 copy_size = src_size ? src_size : r.uncompressed_size;
  if (copy_size > dst_size) {
    if (mapped_res) {
      auto** vt = *(void***)mapped_res;
      ((res_unmap_fn)vt[9])(mapped_res, 0, nullptr);
    }
    return k_e_invalidarg;
  }

  long hr = k_s_ok;
  if (copy_size > 0) {
    if (src_type == 0) {
      std::string path;
      u64 fsz = 0;
      bool fclosed = false;
      {
        std::lock_guard<std::mutex> lk(file_obj->mu);
        path = file_obj->host_path;
        fsz = file_obj->file_size;
        fclosed = file_obj->closed;
      }
      if (fclosed || file_off > fsz || (u64)copy_size > fsz - file_off) {
        hr = k_e_invalidarg;
      } else {
        FILE* fp = fopen(path.c_str(), "rb");
        if (!fp) {
          hr = k_e_file_not_found;
        } else {
#if defined(_WIN32)
          const int sk = _fseeki64(fp, (long long)file_off, SEEK_SET);
#else
          const int sk = fseeko(fp, (off_t)file_off, SEEK_SET);
#endif
          const size_t got = (sk == 0) ? fread(dst_ptr, 1, copy_size, fp) : 0;
          fclose(fp);
          if (sk != 0 || got != copy_size) hr = k_e_fail;
        }
      }
    } else {
      memcpy(dst_ptr, mem_src, copy_size);
    }
  }

  if (mapped_res) {
    auto** vt = *(void***)mapped_res;
    ((res_unmap_fn)vt[9])(mapped_res, 0, nullptr);
  }
  return hr;
}

static void release_cmd_refs(ds_queue_cmd& cmd) {
  if (cmd.retained_file) {
    com_release_any(cmd.retained_file);
    cmd.retained_file = nullptr;
  }
  if (cmd.status_arr) {
    com_release_any(cmd.status_arr);
    cmd.status_arr = nullptr;
  }
  if (cmd.fence) {
    com_release_any(cmd.fence);
    cmd.fence = nullptr;
  }
}

static void queue_worker_loop(ds_queue* q) {
  for (;;) {
    ds_queue_cmd cmd;
    {
      std::unique_lock<std::mutex> lk(q->mu);
      q->cv.wait(lk, [&] { return q->stop_worker || !q->pending.empty(); });
      if (q->stop_worker && q->pending.empty()) break;
      cmd = q->pending.front();
      q->pending.pop_front();
    }

    if (cmd.kind == ds_queue_cmd::cmd_request) {
      const long hr = execute_request(cmd);
      if (hr < 0) {
        u64 ev = 0;
        {
          std::lock_guard<std::mutex> lk(q->mu);
          if (q->batch_hr == k_s_ok) q->batch_hr = hr;
          if (q->failure_count == 0) {
            q->first_failure_hr = hr;
            q->first_failure_cmd = 0;  // DSTORAGE_COMMAND_TYPE_REQUEST
          }
          ++q->failure_count;
          ev = q->error_event;
        }
        if (ev) trt_SetEvent(ev);
      }
    } else if (cmd.kind == ds_queue_cmd::cmd_status) {
      long hr_out = k_s_ok;
      {
        std::lock_guard<std::mutex> lk(q->mu);
        hr_out = q->batch_hr;
        q->batch_hr = k_s_ok;
      }
      if (cmd.status_arr) {
        std::lock_guard<std::mutex> lk(cmd.status_arr->mu);
        if (cmd.status_idx < cmd.status_arr->slots.size())
          cmd.status_arr->slots[cmd.status_idx] = hr_out;
      }
    } else if (cmd.kind == ds_queue_cmd::cmd_signal) {
      if (cmd.fence) {
        auto** vt = *(void***)cmd.fence;
        ((fence_signal_fn)vt[10])(cmd.fence, cmd.fence_value);
      }
    } else if (cmd.kind == ds_queue_cmd::cmd_event) {
      if (cmd.event_handle) trt_SetEvent(cmd.event_handle);
    }

    release_cmd_refs(cmd);
  }
}

static long TRT_GUEST_ABI queue_qi(void* self, const ds_guid* riid, void** ppv) {
  if (!ppv) return k_e_pointer;
  *ppv = nullptr;
  if (!riid) return k_e_invalidarg;
  if (guid_eq(riid, k_iid_unknown) || guid_eq(riid, k_iid_queue) ||
      guid_eq(riid, k_iid_queue1) || guid_eq(riid, k_iid_queue2)) {
    *ppv = self;
    com_addref_any(self);
    return k_s_ok;
  }
  return k_e_nointerface;
}

static u32 TRT_GUEST_ABI queue_addref(void* self) {
  return ++((ds_queue*)self)->ref_count;
}

static void TRT_GUEST_ABI queue_close(void* self) {
  auto* q = (ds_queue*)self;
  {
    std::lock_guard<std::mutex> lk(q->mu);
    q->closed = true;
    q->stop_worker = true;
  }
  q->cv.notify_all();
  if (q->worker.joinable()) q->worker.join();
  std::lock_guard<std::mutex> lk(q->mu);
  while (!q->pending.empty()) {
    release_cmd_refs(q->pending.front());
    q->pending.pop_front();
  }
}

static u32 TRT_GUEST_ABI queue_release(void* self) {
  auto* q = (ds_queue*)self;
  const u32 n = --q->ref_count;
  if (n == 0) {
    queue_close(q);
    if (q->error_event) trt_CloseHandle((void*)(uintptr_t)q->error_event);
    delete q;
  }
  return n;
}

static void TRT_GUEST_ABI queue_enqueue_request(void* self, const ds_raw_request* req) {
  auto* q = (ds_queue*)self;
  if (!req) return;
  ds_queue_cmd cmd;
  cmd.kind = ds_queue_cmd::cmd_request;
  cmd.req = *req;
  if (req_source_type(*req) == 0) {
    ds_file* f = nullptr;
    memcpy(&f, req->source + 0, 8);
    if (f) {
      com_addref_any(f);
      cmd.retained_file = f;
    }
  }
  bool wake = false;
  {
    std::lock_guard<std::mutex> lk(q->mu);
    if (q->closed) {
      release_cmd_refs(cmd);
      return;
    }
    q->pending.push_back(cmd);
    wake = q->pending.size() >= (size_t)(q->desc.capacity / 2);
  }
  if (wake) q->cv.notify_one();
}

static void TRT_GUEST_ABI queue_enqueue_status(void* self, void* status_array, u32 index) {
  auto* q = (ds_queue*)self;
  auto* sa = (ds_status_array*)status_array;
  if (sa) {
    {
      std::lock_guard<std::mutex> lk(sa->mu);
      if (index < sa->slots.size()) sa->slots[index] = k_e_pending;
    }
    com_addref_any(sa);
  }
  ds_queue_cmd cmd;
  cmd.kind = ds_queue_cmd::cmd_status;
  cmd.status_arr = sa;
  cmd.status_idx = index;
  std::lock_guard<std::mutex> lk(q->mu);
  if (q->closed) {
    release_cmd_refs(cmd);
    return;
  }
  q->pending.push_back(cmd);
}

static void TRT_GUEST_ABI queue_enqueue_signal(void* self, void* fence, u64 value) {
  auto* q = (ds_queue*)self;
  if (fence) com_addref_any(fence);
  ds_queue_cmd cmd;
  cmd.kind = ds_queue_cmd::cmd_signal;
  cmd.fence = fence;
  cmd.fence_value = value;
  std::lock_guard<std::mutex> lk(q->mu);
  if (q->closed) {
    release_cmd_refs(cmd);
    return;
  }
  q->pending.push_back(cmd);
}

static void TRT_GUEST_ABI queue_submit(void* self) {
  auto* q = (ds_queue*)self;
  q->cv.notify_all();
}

static void TRT_GUEST_ABI queue_cancel_tag(void* self, u64 mask, u64 value) {
  auto* q = (ds_queue*)self;
  std::lock_guard<std::mutex> lk(q->mu);
  for (auto it = q->pending.begin(); it != q->pending.end();) {
    if (it->kind == ds_queue_cmd::cmd_request &&
        (it->req.cancellation_tag & mask) == value) {
      release_cmd_refs(*it);
      it = q->pending.erase(it);
    } else {
      ++it;
    }
  }
}

static void* TRT_GUEST_ABI queue_get_error_event(void* self) {
  auto* q = (ds_queue*)self;
  std::lock_guard<std::mutex> lk(q->mu);
  if (!q->error_event) q->error_event = trt_CreateEventW(nullptr, true, false, nullptr);
  return (void*)(uintptr_t)q->error_event;
}

static void TRT_GUEST_ABI queue_retrieve_error(void* self, void* record) {
  auto* q = (ds_queue*)self;
  if (!record) return;
  // DSTORAGE_ERROR_RECORD starts with UINT32 FailureCount + DSTORAGE_ERROR_FIRST_FAILURE { HRESULT, CommandType, ... }
  memset(record, 0, 16);
  std::lock_guard<std::mutex> lk(q->mu);
  memcpy((u8*)record + 0, &q->failure_count, 4);
  if (q->failure_count > 0) {
    memcpy((u8*)record + 8, &q->first_failure_hr, 4);
    memcpy((u8*)record + 12, &q->first_failure_cmd, 4);
    q->failure_count = 0;
    q->first_failure_hr = k_s_ok;
    q->first_failure_cmd = -1;
  }
}

static void TRT_GUEST_ABI queue_query(void* self, ds_queue_info* info) {
  auto* q = (ds_queue*)self;
  if (!info) return;
  std::lock_guard<std::mutex> lk(q->mu);
  info->desc = q->desc;
  info->desc.name = q->name_copy.empty() ? nullptr : q->name_copy.c_str();
  const size_t used = q->pending.size();
  const u16 max_avail = q->desc.capacity > 0 ? (u16)(q->desc.capacity - 1u) : 0u;
  info->empty_slot_count = (used >= max_avail) ? 0u : (u16)(max_avail - used);
  const size_t half = q->desc.capacity / 2u;
  info->requests_until_auto_submit = (used >= half) ? 0u : (u16)(half - used);
}

static void TRT_GUEST_ABI queue_enqueue_set_event(void* self, void* handle) {
  auto* q = (ds_queue*)self;
  ds_queue_cmd cmd;
  cmd.kind = ds_queue_cmd::cmd_event;
  cmd.event_handle = (u64)(uintptr_t)handle;
  std::lock_guard<std::mutex> lk(q->mu);
  if (q->closed) return;
  q->pending.push_back(cmd);
}

static u32 TRT_GUEST_ABI queue_get_compression_support(void*, u8) {
  return 0;  // DSTORAGE_COMPRESSION_SUPPORT_NONE
}

static const void* const k_queue_vtbl[] = {
    (const void*)&queue_qi,
    (const void*)&queue_addref,
    (const void*)&queue_release,
    (const void*)&queue_enqueue_request,
    (const void*)&queue_enqueue_status,
    (const void*)&queue_enqueue_signal,
    (const void*)&queue_submit,
    (const void*)&queue_cancel_tag,
    (const void*)&queue_close,
    (const void*)&queue_get_error_event,
    (const void*)&queue_retrieve_error,
    (const void*)&queue_query,
    (const void*)&queue_enqueue_set_event,
    (const void*)&queue_get_compression_support,
};

// ---- IDStorageFactory -------------------------------------------------------

static long TRT_GUEST_ABI factory_qi(void* self, const ds_guid* riid, void** ppv) {
  if (!ppv) return k_e_pointer;
  *ppv = nullptr;
  if (!riid) return k_e_invalidarg;
  if (guid_eq(riid, k_iid_unknown) || guid_eq(riid, k_iid_factory)) {
    *ppv = self;
    com_addref_any(self);
    return k_s_ok;
  }
  return k_e_nointerface;
}

static u32 TRT_GUEST_ABI factory_addref(void* self) {
  return ++((ds_factory*)self)->ref_count;
}

static u32 TRT_GUEST_ABI factory_release(void* self) {
  auto* f = (ds_factory*)self;
  const u32 cur = f->ref_count.load();
  if (cur <= 1) return 1;  // factory singleton stays live for process lifetime
  return --f->ref_count;
}

static long TRT_GUEST_ABI factory_create_queue(void* self, const ds_queue_desc* desc,
                                               const ds_guid* riid, void** ppv) {
  (void)self;
  if (!ppv) return k_e_pointer;
  *ppv = nullptr;
  if (!desc || !riid) return k_e_invalidarg;
  if (desc->capacity < 0x80u || desc->capacity > 0x2000u)
    return k_e_dstorage_invalid_queue_cap;
  if (!guid_eq(riid, k_iid_unknown) && !guid_eq(riid, k_iid_queue) &&
      !guid_eq(riid, k_iid_queue1) && !guid_eq(riid, k_iid_queue2))
    return k_e_nointerface;

  auto* q = new (std::nothrow) ds_queue();
  if (!q) return k_e_outofmemory;
  q->vtbl = k_queue_vtbl;
  q->desc = *desc;
  if (desc->name) q->name_copy = desc->name;
  q->desc.name = q->name_copy.empty() ? nullptr : q->name_copy.c_str();
  q->worker = std::thread(queue_worker_loop, q);
  *ppv = q;
  return k_s_ok;
}

static long TRT_GUEST_ABI factory_open_file(void* self, const u16* path,
                                            const ds_guid* riid, void** ppv) {
  (void)self;
  if (!ppv) return k_e_pointer;
  *ppv = nullptr;
  if (!path || !path[0] || !riid) return k_e_invalidarg;
  if (!guid_eq(riid, k_iid_unknown) && !guid_eq(riid, k_iid_file))
    return k_e_nointerface;

  const std::string host = resolve_dstorage_path(path);
#if defined(_WIN32)
  struct _stat64 st {};
  const bool ok = _stat64(host.c_str(), &st) == 0 && (st.st_mode & _S_IFREG);
#else
  struct stat st {};
  const bool ok = stat(host.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#endif
  if (!ok) return k_e_file_not_found;

  auto* f = new (std::nothrow) ds_file();
  if (!f) return k_e_outofmemory;
  f->vtbl = k_file_vtbl;
  f->host_path = host;
  f->file_size = (u64)st.st_size;
  *ppv = f;
  return k_s_ok;
}

static long TRT_GUEST_ABI factory_create_status_array(void* self, u32 capacity,
                                                      const char* name,
                                                      const ds_guid* riid,
                                                      void** ppv) {
  (void)self;
  if (!ppv) return k_e_pointer;
  *ppv = nullptr;
  if (capacity == 0 || !riid) return k_e_invalidarg;
  if (!guid_eq(riid, k_iid_unknown) && !guid_eq(riid, k_iid_status_array))
    return k_e_nointerface;

  auto* sa = new (std::nothrow) ds_status_array();
  if (!sa) return k_e_outofmemory;
  sa->vtbl = k_status_vtbl;
  sa->slots.assign(capacity, k_s_ok);
  if (name) sa->name = name;
  *ppv = sa;
  return k_s_ok;
}

static void TRT_GUEST_ABI factory_set_debug_flags(void* self, u32 flags) {
  ((ds_factory*)self)->debug_flags = flags;
}

static long TRT_GUEST_ABI factory_set_staging_size(void* self, u32 size) {
  ((ds_factory*)self)->staging_size = size;
  return k_s_ok;
}

static const void* const k_factory_vtbl[] = {
    (const void*)&factory_qi,
    (const void*)&factory_addref,
    (const void*)&factory_release,
    (const void*)&factory_create_queue,
    (const void*)&factory_open_file,
    (const void*)&factory_create_status_array,
    (const void*)&factory_set_debug_flags,
    (const void*)&factory_set_staging_size,
};

static ds_factory g_factory{k_factory_vtbl};

}  // namespace

extern "C" {

long TRT_GUEST_ABI dstorage_DStorageGetFactory(const void* riid, void** ppv) {
  if (!ppv) return k_e_pointer;
  *ppv = nullptr;
  if (!riid) return k_e_invalidarg;
  const auto* gid = (const ds_guid*)riid;
  if (!guid_eq(gid, k_iid_unknown) && !guid_eq(gid, k_iid_factory))
    return k_e_nointerface;
  ++g_factory.ref_count;
  *ppv = &g_factory;
  return k_s_ok;
}

long TRT_GUEST_ABI dstorage_DStorageSetConfiguration(const void* config) {
  (void)config;
  return k_s_ok;
}

long TRT_GUEST_ABI dstorage_DStorageSetConfiguration1(const void* config) {
  (void)config;
  return k_s_ok;
}

long TRT_GUEST_ABI dstorage_DStorageCreateCompressionCodec(u32 format, u32 threads,
                                                          const void* riid,
                                                          void** ppv) {
  (void)format;
  (void)threads;
  (void)riid;
  if (ppv) *ppv = nullptr;
  return k_e_notimpl;
}

}  // extern "C"

extern "C" int trt_bridge_register_dstorage(void) {
  static const trt_named_fn k_fns[] = {
      {"DStorageGetFactory", (void*)&dstorage_DStorageGetFactory},
      {"DStorageSetConfiguration", (void*)&dstorage_DStorageSetConfiguration},
      {"DStorageSetConfiguration1", (void*)&dstorage_DStorageSetConfiguration1},
      {"DStorageCreateCompressionCodec", (void*)&dstorage_DStorageCreateCompressionCodec},
  };
  trt_module_register("dstorage.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  trt_module_register("dstoragecore.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  return 0;
}
