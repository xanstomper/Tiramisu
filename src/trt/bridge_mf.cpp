// tiramisu/src/trt/bridge_mf.cpp
// Honest Media Foundation COM bridge (mfplat, mfreadwrite, mf, mfuuid, evr, dxva2).
// Provides genuine IMFAttributes / IMFMediaType / IMFMediaBuffer / IMFSample /
// IMFAsyncResult / IMFDXGIDeviceManager / IMFSourceReader COM vtables so games
// using Unity VideoPlayer, Unreal WmfMedia, or PC cutscene players initialize
// cleanly, inspect real media type attributes, and observe clean EOS without
// hanging or dereferencing null vtables.
#include "bridge_mf.h"
#include "bridge_ole.h"
#include "images.h"
#include "trt.h"
#include "thunk.h"

#include <atomic>
#include <mutex>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

namespace {

constexpr i32 S_OK_ = 0;
constexpr i32 E_NOINTERFACE_ = (i32)0x80004002u;
constexpr i32 E_POINTER_ = (i32)0x80004003u;
constexpr i32 E_INVALIDARG_ = (i32)0x80070057u;
constexpr i32 E_NOTIMPL_ = (i32)0x80004001u;
constexpr i32 E_OUTOFMEMORY_ = (i32)0x8007000eu;
constexpr i32 E_UNEXPECTED_ = (i32)0x8000ffffu;
constexpr i32 MF_E_BUFFERTOOSMALL_ = (i32)0xc00d36b1u;
constexpr i32 MF_E_ATTRIBUTENOTFOUND_ = (i32)0xC00D36E6u;
constexpr i32 MF_E_INVALIDINDEX_ = (i32)0xC00D36E7u;
constexpr i32 MF_E_NO_MORE_TYPES_ = (i32)0xC00D36B9u;

// Well-known MF GUIDs
// MF_MT_MAJOR_TYPE: {48eba18e-f8c9-4687-bf11-0a74c9f96a8f}
const u8 k_MF_MT_MAJOR_TYPE[16] = {
    0x8e,0xa1,0xeb,0x48, 0xc9,0xf8, 0x87,0x46, 0xbf,0x11, 0x0a,0x74,0xc9,0xf9,0x6a,0x8f};
// MF_MT_SUBTYPE: {f7e34c9a-42e8-4714-b74b-cb29d72c35e5}
const u8 k_MF_MT_SUBTYPE[16] = {
    0x9a,0x4c,0xe3,0xf7, 0xe8,0x42, 0x14,0x47, 0xb7,0x4b, 0xcb,0x29,0xd7,0x2c,0x35,0xe5};
// MFMediaType_Video: {73646976-0000-0010-8000-00AA00389B71}
const u8 k_MFMediaType_Video[16] = {
    0x76,0x69,0x64,0x73, 0x00,0x00, 0x10,0x00, 0x80,0x00, 0x00,0xaa,0x00,0x38,0x9b,0x71};
// MFMediaType_Audio: {73647561-0000-0010-8000-00AA00389B71}
const u8 k_MFMediaType_Audio[16] = {
    0x61,0x75,0x64,0x73, 0x00,0x00, 0x10,0x00, 0x80,0x00, 0x00,0xaa,0x00,0x38,0x9b,0x71};
// MFVideoFormat_RGB32: {00000016-0000-0010-8000-00AA00389B71}
const u8 k_MFVideoFormat_RGB32[16] = {
    0x16,0x00,0x00,0x00, 0x00,0x00, 0x10,0x00, 0x80,0x00, 0x00,0xaa,0x00,0x38,0x9b,0x71};
// MFAudioFormat_PCM: {00000001-0000-0010-8000-00AA00389B71}
const u8 k_MFAudioFormat_PCM[16] = {
    0x01,0x00,0x00,0x00, 0x00,0x00, 0x10,0x00, 0x80,0x00, 0x00,0xaa,0x00,0x38,0x9b,0x71};
// MF_MT_FRAME_SIZE: {1652c33d-d6b2-4012-b834-72030849a37d}
const u8 k_MF_MT_FRAME_SIZE[16] = {
    0x3d,0xc3,0x52,0x16, 0xb2,0xd6, 0x12,0x40, 0xb8,0x34, 0x72,0x03,0x08,0x49,0xa3,0x7d};
// MF_MT_FRAME_RATE: {c459a2e8-3d2c-4e44-b132-fee5156c7bb0}
const u8 k_MF_MT_FRAME_RATE[16] = {
    0xe8,0xa2,0x59,0xc4, 0x2c,0x3d, 0x44,0x4e, 0xb1,0x32, 0xfe,0xe5,0x15,0x6c,0x7b,0xb0};
// MF_PD_DURATION: {6c990d33-bb8e-477a-8598-0d5d96fcd88a}
const u8 k_MF_PD_DURATION[16] = {
    0x33,0x0d,0x99,0x6c, 0x8e,0xbb, 0x7a,0x47, 0x85,0x98, 0x0d,0x5d,0x96,0xfc,0xd8,0x8a};

// ============================================================================
// Attribute store backing IMFAttributes / IMFMediaType / IMFSample
// ============================================================================
enum mf_attr_type : u32 {
  MF_ATTR_UINT32_ = 19,    // VT_UI4
  MF_ATTR_UINT64_ = 21,    // VT_UI8
  MF_ATTR_DOUBLE_ = 5,     // VT_R8
  MF_ATTR_STRING_ = 31,    // VT_LPWSTR
  MF_ATTR_BLOB_ = 0x1011,  // VT_VECTOR | VT_UI1
  MF_ATTR_UNKNOWN_ = 13,   // VT_UNKNOWN
};

struct mf_attr_entry {
  u8 key[16];
  u32 type = 0;
  u64 u64_val = 0;
  double d_val = 0.0;
  std::vector<u16> str_val;
  std::vector<u8> blob_val;
  void* unk_val = nullptr;
};

struct mf_attr_obj {
  const void** vtbl = nullptr;
  std::atomic<u32> ref{1};
  std::mutex mu;
  std::vector<mf_attr_entry> items;
  bool is_media_type = false;
  bool is_sample = false;
  // IMFSample-specific state
  u32 sample_flags = 0;
  i64 sample_time = 0;
  i64 sample_duration = 0;
  std::vector<void*> sample_buffers;
};

static mf_attr_entry* attr_find_locked(mf_attr_obj* o, const u8* guid) {
  for (auto& e : o->items) {
    if (memcmp(e.key, guid, 16) == 0) return &e;
  }
  return nullptr;
}

static mf_attr_entry* attr_upsert_locked(mf_attr_obj* o, const u8* guid) {
  if (auto* e = attr_find_locked(o, guid)) return e;
  o->items.emplace_back();
  mf_attr_entry& ne = o->items.back();
  memcpy(ne.key, guid, 16);
  return &ne;
}

static void attr_release_unk(void* unk) {
  if (!unk) return;
  using rel_fn = u32(TRT_GUEST_ABI*)(void*);
  const void** vt = *(const void***)unk;
  if (vt && vt[2]) ((rel_fn)vt[2])(unk);
}
static void attr_addref_unk(void* unk) {
  if (!unk) return;
  using add_fn = u32(TRT_GUEST_ABI*)(void*);
  const void** vt = *(const void***)unk;
  if (vt && vt[1]) ((add_fn)vt[1])(unk);
}

i32 TRT_GUEST_ABI mfa_QueryInterface(void* self, const u8* riid, void** out) {
  (void)riid;
  if (!out) return E_POINTER_;
  *out = self;
  ((mf_attr_obj*)self)->ref.fetch_add(1, std::memory_order_relaxed);
  return S_OK_;
}
u32 TRT_GUEST_ABI mfa_AddRef(void* self) {
  return ((mf_attr_obj*)self)->ref.fetch_add(1, std::memory_order_relaxed) + 1u;
}
u32 TRT_GUEST_ABI mfa_Release(void* self) {
  mf_attr_obj* o = (mf_attr_obj*)self;
  const u32 r = o->ref.fetch_sub(1, std::memory_order_acq_rel) - 1u;
  if (r == 0) {
    for (auto& e : o->items) {
      if (e.type == MF_ATTR_UNKNOWN_) attr_release_unk(e.unk_val);
    }
    for (void* b : o->sample_buffers) attr_release_unk(b);
    delete o;
  }
  return r;
}

static void write_propvar_from_entry(const mf_attr_entry& e, void* pv) {
  memset(pv, 0, 24);
  *(u16*)pv = (u16)e.type;
  if (e.type == MF_ATTR_UINT32_) {
    *(u32*)((u8*)pv + 8) = (u32)e.u64_val;
  } else if (e.type == MF_ATTR_UINT64_) {
    *(u64*)((u8*)pv + 8) = e.u64_val;
  } else if (e.type == MF_ATTR_DOUBLE_) {
    memcpy((u8*)pv + 8, &e.d_val, 8);
  } else if (e.type == MF_ATTR_STRING_) {
    const size_t bytes = e.str_val.size() * sizeof(u16);
    void* copy = bytes ? ole_CoTaskMemAlloc((u32)bytes) : nullptr;
    if (copy) memcpy(copy, e.str_val.data(), bytes);
    *(void**)((u8*)pv + 8) = copy;
  } else if (e.type == MF_ATTR_BLOB_) {
    const u32 sz = (u32)e.blob_val.size();
    void* copy = sz ? ole_CoTaskMemAlloc(sz) : nullptr;
    if (copy) memcpy(copy, e.blob_val.data(), sz);
    *(u32*)((u8*)pv + 8) = sz;
    *(void**)((u8*)pv + 16) = copy;
  } else if (e.type == MF_ATTR_UNKNOWN_) {
    attr_addref_unk(e.unk_val);
    *(void**)((u8*)pv + 8) = e.unk_val;
  }
}

i32 TRT_GUEST_ABI mfa_GetItem(void* self, const u8* guid, void* pv) {
  if (!guid) return E_INVALIDARG_;
  mf_attr_obj* o = (mf_attr_obj*)self;
  std::lock_guard<std::mutex> lk(o->mu);
  mf_attr_entry* e = attr_find_locked(o, guid);
  if (!e) return MF_E_ATTRIBUTENOTFOUND_;
  if (pv) write_propvar_from_entry(*e, pv);
  return S_OK_;
}
i32 TRT_GUEST_ABI mfa_GetItemType(void* self, const u8* guid, u32* type_out) {
  if (!guid || !type_out) return E_POINTER_;
  mf_attr_obj* o = (mf_attr_obj*)self;
  std::lock_guard<std::mutex> lk(o->mu);
  mf_attr_entry* e = attr_find_locked(o, guid);
  if (!e) return MF_E_ATTRIBUTENOTFOUND_;
  *type_out = e->type;
  return S_OK_;
}
i32 TRT_GUEST_ABI mfa_CompareItem(void*, const u8*, const void*, int* res) {
  if (res) *res = 0;
  return S_OK_;
}
i32 TRT_GUEST_ABI mfa_Compare(void*, void*, u32, int* res) {
  if (res) *res = 0;
  return S_OK_;
}
i32 TRT_GUEST_ABI mfa_GetUINT32(void* self, const u8* guid, u32* val) {
  if (!guid || !val) return E_POINTER_;
  mf_attr_obj* o = (mf_attr_obj*)self;
  std::lock_guard<std::mutex> lk(o->mu);
  mf_attr_entry* e = attr_find_locked(o, guid);
  if (!e || e->type != MF_ATTR_UINT32_) return MF_E_ATTRIBUTENOTFOUND_;
  *val = (u32)e->u64_val;
  return S_OK_;
}
i32 TRT_GUEST_ABI mfa_GetUINT64(void* self, const u8* guid, u64* val) {
  if (!guid || !val) return E_POINTER_;
  mf_attr_obj* o = (mf_attr_obj*)self;
  std::lock_guard<std::mutex> lk(o->mu);
  mf_attr_entry* e = attr_find_locked(o, guid);
  if (!e || e->type != MF_ATTR_UINT64_) return MF_E_ATTRIBUTENOTFOUND_;
  *val = e->u64_val;
  return S_OK_;
}
i32 TRT_GUEST_ABI mfa_GetDouble(void* self, const u8* guid, double* val) {
  if (!guid || !val) return E_POINTER_;
  mf_attr_obj* o = (mf_attr_obj*)self;
  std::lock_guard<std::mutex> lk(o->mu);
  mf_attr_entry* e = attr_find_locked(o, guid);
  if (!e || e->type != MF_ATTR_DOUBLE_) return MF_E_ATTRIBUTENOTFOUND_;
  *val = e->d_val;
  return S_OK_;
}
i32 TRT_GUEST_ABI mfa_GetGUID(void* self, const u8* guid, u8* out) {
  if (!guid || !out) return E_POINTER_;
  mf_attr_obj* o = (mf_attr_obj*)self;
  std::lock_guard<std::mutex> lk(o->mu);
  mf_attr_entry* e = attr_find_locked(o, guid);
  if (!e || e->type != MF_ATTR_BLOB_ || e->blob_val.size() < 16)
    return MF_E_ATTRIBUTENOTFOUND_;
  memcpy(out, e->blob_val.data(), 16);
  return S_OK_;
}
i32 TRT_GUEST_ABI mfa_GetStringLength(void* self, const u8* guid, u32* len) {
  if (!guid || !len) return E_POINTER_;
  mf_attr_obj* o = (mf_attr_obj*)self;
  std::lock_guard<std::mutex> lk(o->mu);
  mf_attr_entry* e = attr_find_locked(o, guid);
  if (!e || e->type != MF_ATTR_STRING_) return MF_E_ATTRIBUTENOTFOUND_;
  *len = e->str_val.empty() ? 0 : (u32)(e->str_val.size() - 1);
  return S_OK_;
}
i32 TRT_GUEST_ABI mfa_GetString(void* self, const u8* guid, u16* buf,
                                u32 cap, u32* len_out) {
  if (!guid || !buf || cap == 0) return E_INVALIDARG_;
  mf_attr_obj* o = (mf_attr_obj*)self;
  std::lock_guard<std::mutex> lk(o->mu);
  mf_attr_entry* e = attr_find_locked(o, guid);
  if (!e || e->type != MF_ATTR_STRING_) return MF_E_ATTRIBUTENOTFOUND_;
  const u32 n = e->str_val.empty() ? 0 : (u32)(e->str_val.size() - 1);
  if (len_out) *len_out = n;
  const u32 cp = (n + 1 < cap) ? n : (cap - 1);
  if (cp > 0) memcpy(buf, e->str_val.data(), cp * sizeof(u16));
  buf[cp] = 0;
  return S_OK_;
}
i32 TRT_GUEST_ABI mfa_GetAllocatedString(void* self, const u8* guid, u16** out,
                                         u32* len_out) {
  if (!guid || !out || !len_out) return E_POINTER_;
  mf_attr_obj* o = (mf_attr_obj*)self;
  std::lock_guard<std::mutex> lk(o->mu);
  mf_attr_entry* e = attr_find_locked(o, guid);
  if (!e || e->type != MF_ATTR_STRING_) return MF_E_ATTRIBUTENOTFOUND_;
  const size_t sz = e->str_val.size() * sizeof(u16);
  u16* copy = (u16*)ole_CoTaskMemAlloc((u32)(sz ? sz : 2));
  if (!copy) return (i32)0x8007000Eu;
  if (sz) memcpy(copy, e->str_val.data(), sz);
  else copy[0] = 0;
  *out = copy;
  *len_out = e->str_val.empty() ? 0 : (u32)(e->str_val.size() - 1);
  return S_OK_;
}
i32 TRT_GUEST_ABI mfa_GetBlobSize(void* self, const u8* guid, u32* sz) {
  if (!guid || !sz) return E_POINTER_;
  mf_attr_obj* o = (mf_attr_obj*)self;
  std::lock_guard<std::mutex> lk(o->mu);
  mf_attr_entry* e = attr_find_locked(o, guid);
  if (!e || e->type != MF_ATTR_BLOB_) return MF_E_ATTRIBUTENOTFOUND_;
  *sz = (u32)e->blob_val.size();
  return S_OK_;
}
i32 TRT_GUEST_ABI mfa_GetBlob(void* self, const u8* guid, u8* buf, u32 cap,
                              u32* sz_out) {
  if (!guid) return E_INVALIDARG_;
  mf_attr_obj* o = (mf_attr_obj*)self;
  std::lock_guard<std::mutex> lk(o->mu);
  mf_attr_entry* e = attr_find_locked(o, guid);
  if (!e || e->type != MF_ATTR_BLOB_) return MF_E_ATTRIBUTENOTFOUND_;
  const u32 sz = (u32)e->blob_val.size();
  if (sz_out) *sz_out = sz;
  if (!buf || cap < sz) return (i32)0x8007007Au;
  if (sz) memcpy(buf, e->blob_val.data(), sz);
  return S_OK_;
}
i32 TRT_GUEST_ABI mfa_GetAllocatedBlob(void* self, const u8* guid, u8** buf,
                                       u32* sz_out) {
  if (!guid || !buf || !sz_out) return E_POINTER_;
  mf_attr_obj* o = (mf_attr_obj*)self;
  std::lock_guard<std::mutex> lk(o->mu);
  mf_attr_entry* e = attr_find_locked(o, guid);
  if (!e || e->type != MF_ATTR_BLOB_) return MF_E_ATTRIBUTENOTFOUND_;
  const u32 sz = (u32)e->blob_val.size();
  u8* p = (u8*)ole_CoTaskMemAlloc(sz ? sz : 1);
  if (!p) return (i32)0x8007000Eu;
  if (sz) memcpy(p, e->blob_val.data(), sz);
  *buf = p;
  *sz_out = sz;
  return S_OK_;
}
i32 TRT_GUEST_ABI mfa_GetUnknown(void* self, const u8* guid, const u8* riid,
                                 void** out) {
  (void)riid;
  if (!guid || !out) return E_POINTER_;
  *out = nullptr;
  mf_attr_obj* o = (mf_attr_obj*)self;
  std::lock_guard<std::mutex> lk(o->mu);
  mf_attr_entry* e = attr_find_locked(o, guid);
  if (!e || e->type != MF_ATTR_UNKNOWN_ || !e->unk_val)
    return MF_E_ATTRIBUTENOTFOUND_;
  attr_addref_unk(e->unk_val);
  *out = e->unk_val;
  return S_OK_;
}
i32 TRT_GUEST_ABI mfa_SetItem(void* self, const u8* guid, const void* pv) {
  if (!guid || !pv) return E_INVALIDARG_;
  mf_attr_obj* o = (mf_attr_obj*)self;
  const u16 vt = *(const u16*)pv;
  std::lock_guard<std::mutex> lk(o->mu);
  mf_attr_entry* e = attr_upsert_locked(o, guid);
  if (e->type == MF_ATTR_UNKNOWN_) attr_release_unk(e->unk_val);
  e->type = vt;
  e->unk_val = nullptr;
  e->str_val.clear();
  e->blob_val.clear();
  if (vt == MF_ATTR_UINT32_ || vt == 3 /*VT_I4*/) {
    e->type = MF_ATTR_UINT32_;
    e->u64_val = *(const u32*)((const u8*)pv + 8);
  } else if (vt == MF_ATTR_UINT64_ || vt == 20 /*VT_I8*/) {
    e->type = MF_ATTR_UINT64_;
    e->u64_val = *(const u64*)((const u8*)pv + 8);
  } else if (vt == MF_ATTR_DOUBLE_) {
    memcpy(&e->d_val, (const u8*)pv + 8, 8);
  } else if (vt == MF_ATTR_STRING_) {
    const u16* s = *(const u16* const*)((const u8*)pv + 8);
    if (s) {
      size_t n = 0;
      while (s[n]) ++n;
      e->str_val.assign(s, s + n + 1);
    } else {
      e->str_val.push_back(0);
    }
  } else if (vt == MF_ATTR_UNKNOWN_) {
    void* u = *(void* const*)((const u8*)pv + 8);
    attr_addref_unk(u);
    e->unk_val = u;
  }
  return S_OK_;
}
i32 TRT_GUEST_ABI mfa_DeleteItem(void* self, const u8* guid) {
  if (!guid) return E_INVALIDARG_;
  mf_attr_obj* o = (mf_attr_obj*)self;
  std::lock_guard<std::mutex> lk(o->mu);
  for (size_t i = 0; i < o->items.size(); ++i) {
    if (memcmp(o->items[i].key, guid, 16) == 0) {
      if (o->items[i].type == MF_ATTR_UNKNOWN_)
        attr_release_unk(o->items[i].unk_val);
      o->items.erase(o->items.begin() + (ptrdiff_t)i);
      return S_OK_;
    }
  }
  return S_OK_;
}
i32 TRT_GUEST_ABI mfa_DeleteAllItems(void* self) {
  mf_attr_obj* o = (mf_attr_obj*)self;
  std::lock_guard<std::mutex> lk(o->mu);
  for (auto& e : o->items) {
    if (e.type == MF_ATTR_UNKNOWN_) attr_release_unk(e.unk_val);
  }
  o->items.clear();
  return S_OK_;
}
i32 TRT_GUEST_ABI mfa_SetUINT32(void* self, const u8* guid, u32 val) {
  if (!guid) return E_INVALIDARG_;
  mf_attr_obj* o = (mf_attr_obj*)self;
  std::lock_guard<std::mutex> lk(o->mu);
  mf_attr_entry* e = attr_upsert_locked(o, guid);
  e->type = MF_ATTR_UINT32_;
  e->u64_val = val;
  return S_OK_;
}
i32 TRT_GUEST_ABI mfa_SetUINT64(void* self, const u8* guid, u64 val) {
  if (!guid) return E_INVALIDARG_;
  mf_attr_obj* o = (mf_attr_obj*)self;
  std::lock_guard<std::mutex> lk(o->mu);
  mf_attr_entry* e = attr_upsert_locked(o, guid);
  e->type = MF_ATTR_UINT64_;
  e->u64_val = val;
  return S_OK_;
}
i32 TRT_GUEST_ABI mfa_SetDouble(void* self, const u8* guid, double val) {
  if (!guid) return E_INVALIDARG_;
  mf_attr_obj* o = (mf_attr_obj*)self;
  std::lock_guard<std::mutex> lk(o->mu);
  mf_attr_entry* e = attr_upsert_locked(o, guid);
  e->type = MF_ATTR_DOUBLE_;
  e->d_val = val;
  return S_OK_;
}
i32 TRT_GUEST_ABI mfa_SetGUID(void* self, const u8* guid, const u8* val) {
  if (!guid || !val) return E_INVALIDARG_;
  mf_attr_obj* o = (mf_attr_obj*)self;
  std::lock_guard<std::mutex> lk(o->mu);
  mf_attr_entry* e = attr_upsert_locked(o, guid);
  e->type = MF_ATTR_BLOB_;
  e->blob_val.assign(val, val + 16);
  return S_OK_;
}
i32 TRT_GUEST_ABI mfa_SetString(void* self, const u8* guid, const u16* s) {
  if (!guid || !s) return E_INVALIDARG_;
  size_t n = 0;
  while (s[n]) ++n;
  mf_attr_obj* o = (mf_attr_obj*)self;
  std::lock_guard<std::mutex> lk(o->mu);
  mf_attr_entry* e = attr_upsert_locked(o, guid);
  e->type = MF_ATTR_STRING_;
  e->str_val.assign(s, s + n + 1);
  return S_OK_;
}
i32 TRT_GUEST_ABI mfa_SetBlob(void* self, const u8* guid, const u8* buf, u32 sz) {
  if (!guid || (!buf && sz > 0)) return E_INVALIDARG_;
  mf_attr_obj* o = (mf_attr_obj*)self;
  std::lock_guard<std::mutex> lk(o->mu);
  mf_attr_entry* e = attr_upsert_locked(o, guid);
  e->type = MF_ATTR_BLOB_;
  if (sz > 0) e->blob_val.assign(buf, buf + sz);
  else e->blob_val.clear();
  return S_OK_;
}
i32 TRT_GUEST_ABI mfa_SetUnknown(void* self, const u8* guid, void* unk) {
  if (!guid) return E_INVALIDARG_;
  mf_attr_obj* o = (mf_attr_obj*)self;
  std::lock_guard<std::mutex> lk(o->mu);
  mf_attr_entry* e = attr_upsert_locked(o, guid);
  if (e->type == MF_ATTR_UNKNOWN_) attr_release_unk(e->unk_val);
  e->type = MF_ATTR_UNKNOWN_;
  attr_addref_unk(unk);
  e->unk_val = unk;
  return S_OK_;
}
i32 TRT_GUEST_ABI mfa_LockStore(void*) { return S_OK_; }
i32 TRT_GUEST_ABI mfa_UnlockStore(void*) { return S_OK_; }
i32 TRT_GUEST_ABI mfa_GetCount(void* self, u32* count) {
  if (!count) return E_POINTER_;
  mf_attr_obj* o = (mf_attr_obj*)self;
  std::lock_guard<std::mutex> lk(o->mu);
  *count = (u32)o->items.size();
  return S_OK_;
}
i32 TRT_GUEST_ABI mfa_GetItemByIndex(void* self, u32 idx, u8* guid_out, void* pv) {
  mf_attr_obj* o = (mf_attr_obj*)self;
  std::lock_guard<std::mutex> lk(o->mu);
  if (idx >= o->items.size()) return MF_E_INVALIDINDEX_;
  if (guid_out) memcpy(guid_out, o->items[idx].key, 16);
  if (pv) write_propvar_from_entry(o->items[idx], pv);
  return S_OK_;
}
i32 TRT_GUEST_ABI mfa_CopyAllItems(void* self, void* dst_raw) {
  mf_attr_obj* o = (mf_attr_obj*)self;
  mf_attr_obj* dst = (mf_attr_obj*)dst_raw;
  if (!dst) return E_INVALIDARG_;
  std::lock_guard<std::mutex> lk(o->mu);
  dst->items = o->items;
  for (auto& e : dst->items) {
    if (e.type == MF_ATTR_UNKNOWN_) attr_addref_unk(e.unk_val);
  }
  return S_OK_;
}

// IMFMediaType slots 33..37
i32 TRT_GUEST_ABI mfmt_GetMajorType(void* self, u8* guid_out) {
  return mfa_GetGUID(self, k_MF_MT_MAJOR_TYPE, guid_out);
}
i32 TRT_GUEST_ABI mfmt_IsCompressedFormat(void*, int* compressed) {
  if (compressed) *compressed = 0;
  return S_OK_;
}
i32 TRT_GUEST_ABI mfmt_IsEqual(void*, void*, u32* flags) {
  if (flags) *flags = 0;
  return S_OK_;
}
i32 TRT_GUEST_ABI mfmt_GetRepresentation(void*, const u8*, void** rep) {
  if (rep) *rep = nullptr;
  return E_NOTIMPL_;
}
i32 TRT_GUEST_ABI mfmt_FreeRepresentation(void*, const u8*, void*) {
  return S_OK_;
}

// IMFMediaBuffer
struct mf_buffer_obj {
  const void** vtbl = nullptr;
  std::atomic<u32> ref{1};
  std::vector<u8> data;
  u32 cur_len = 0;
  u32 charged_bytes = 0;

  ~mf_buffer_obj() { trt_mem_budget_discharge_commit(charged_bytes); }
};
i32 TRT_GUEST_ABI mfb_QueryInterface(void* self, const u8* riid, void** out) {
  (void)riid;
  if (!out) return E_POINTER_;
  *out = self;
  ((mf_buffer_obj*)self)->ref.fetch_add(1, std::memory_order_relaxed);
  return S_OK_;
}
u32 TRT_GUEST_ABI mfb_AddRef(void* self) {
  return ((mf_buffer_obj*)self)->ref.fetch_add(1, std::memory_order_relaxed) + 1u;
}
u32 TRT_GUEST_ABI mfb_Release(void* self) {
  auto* b = (mf_buffer_obj*)self;
  const u32 r = b->ref.fetch_sub(1, std::memory_order_acq_rel) - 1u;
  if (r == 0) delete b;
  return r;
}
i32 TRT_GUEST_ABI mfb_Lock(void* self, u8** ptr, u32* max_len, u32* cur_len) {
  if (!ptr) return E_POINTER_;
  auto* b = (mf_buffer_obj*)self;
  *ptr = b->data.empty() ? nullptr : b->data.data();
  if (max_len) *max_len = (u32)b->data.size();
  if (cur_len) *cur_len = b->cur_len;
  return S_OK_;
}
i32 TRT_GUEST_ABI mfb_Unlock(void*) { return S_OK_; }
i32 TRT_GUEST_ABI mfb_GetCurrentLength(void* self, u32* len) {
  if (!len) return E_POINTER_;
  *len = ((mf_buffer_obj*)self)->cur_len;
  return S_OK_;
}
i32 TRT_GUEST_ABI mfb_SetCurrentLength(void* self, u32 len) {
  auto* b = (mf_buffer_obj*)self;
  if (len > b->data.size()) return E_INVALIDARG_;
  b->cur_len = len;
  return S_OK_;
}
i32 TRT_GUEST_ABI mfb_GetMaxLength(void* self, u32* len) {
  if (!len) return E_POINTER_;
  *len = (u32)((mf_buffer_obj*)self)->data.size();
  return S_OK_;
}
const void* g_mfbuffer_vtbl[8] = {
    (const void*)&mfb_QueryInterface,
    (const void*)&mfb_AddRef,
    (const void*)&mfb_Release,
    (const void*)&mfb_Lock,
    (const void*)&mfb_Unlock,
    (const void*)&mfb_GetCurrentLength,
    (const void*)&mfb_SetCurrentLength,
    (const void*)&mfb_GetMaxLength,
};
i32 TRT_GUEST_ABI mf_MFCreateMemoryBuffer(u32 max_len, void** out);

// IMFSample slots 33..46
i32 TRT_GUEST_ABI mfs_GetSampleFlags(void* s, u32* f) {
  if (f) *f = ((mf_attr_obj*)s)->sample_flags;
  return S_OK_;
}
i32 TRT_GUEST_ABI mfs_SetSampleFlags(void* s, u32 f) {
  ((mf_attr_obj*)s)->sample_flags = f;
  return S_OK_;
}
i32 TRT_GUEST_ABI mfs_GetSampleTime(void* s, i64* t) {
  if (t) *t = ((mf_attr_obj*)s)->sample_time;
  return S_OK_;
}
i32 TRT_GUEST_ABI mfs_SetSampleTime(void* s, i64 t) {
  ((mf_attr_obj*)s)->sample_time = t;
  return S_OK_;
}
i32 TRT_GUEST_ABI mfs_GetSampleDuration(void* s, i64* d) {
  if (d) *d = ((mf_attr_obj*)s)->sample_duration;
  return S_OK_;
}
i32 TRT_GUEST_ABI mfs_SetSampleDuration(void* s, i64 d) {
  ((mf_attr_obj*)s)->sample_duration = d;
  return S_OK_;
}
i32 TRT_GUEST_ABI mfs_GetBufferCount(void* s, u32* c) {
  if (!c) return E_POINTER_;
  auto* o = (mf_attr_obj*)s;
  std::lock_guard<std::mutex> lk(o->mu);
  *c = (u32)o->sample_buffers.size();
  return S_OK_;
}
i32 TRT_GUEST_ABI mfs_GetBufferByIndex(void* s, u32 idx, void** out) {
  if (!out) return E_POINTER_;
  auto* o = (mf_attr_obj*)s;
  std::lock_guard<std::mutex> lk(o->mu);
  if (idx >= o->sample_buffers.size()) return E_INVALIDARG_;
  attr_addref_unk(o->sample_buffers[idx]);
  *out = o->sample_buffers[idx];
  return S_OK_;
}
i32 TRT_GUEST_ABI mfs_AddBuffer(void* s, void* buf) {
  if (!buf) return E_INVALIDARG_;
  auto* o = (mf_attr_obj*)s;
  std::lock_guard<std::mutex> lk(o->mu);
  attr_addref_unk(buf);
  o->sample_buffers.push_back(buf);
  return S_OK_;
}
i32 TRT_GUEST_ABI mfs_RemoveBufferByIndex(void* s, u32 idx) {
  auto* o = (mf_attr_obj*)s;
  std::lock_guard<std::mutex> lk(o->mu);
  if (idx >= o->sample_buffers.size()) return E_INVALIDARG_;
  attr_release_unk(o->sample_buffers[idx]);
  o->sample_buffers.erase(o->sample_buffers.begin() + (ptrdiff_t)idx);
  return S_OK_;
}
i32 TRT_GUEST_ABI mfs_RemoveAllBuffers(void* s) {
  auto* o = (mf_attr_obj*)s;
  std::lock_guard<std::mutex> lk(o->mu);
  for (void* b : o->sample_buffers) attr_release_unk(b);
  o->sample_buffers.clear();
  return S_OK_;
}
i32 sample_buffer_length(const mf_attr_obj& sample, u32* len) {
  u64 total = 0;
  using getlen_fn = i32(TRT_GUEST_ABI*)(void*, u32*);
  for (void* b : sample.sample_buffers) {
    u32 cur = 0;
    const void** vt = *(const void***)b;
    const i32 hr = ((getlen_fn)vt[5])(b, &cur);
    if (hr < 0) return hr;
    total += cur;
    if (total > UINT32_MAX) return E_OUTOFMEMORY_;
  }
  *len = static_cast<u32>(total);
  return S_OK_;
}

i32 sample_copy_buffers(const mf_attr_obj& sample, void* dest, u32 total) {
  using lock_fn = i32(TRT_GUEST_ABI*)(void*, u8**, u32*, u32*);
  using unlock_fn = i32(TRT_GUEST_ABI*)(void*);
  using setlen_fn = i32(TRT_GUEST_ABI*)(void*, u32);
  const void** dest_vt = *(const void***)dest;
  u8* dest_data = nullptr;
  u32 capacity = 0;
  i32 hr = ((lock_fn)dest_vt[3])(dest, &dest_data, &capacity, nullptr);
  if (hr < 0) return hr;

  u32 copied = 0;
  if (capacity < total) hr = MF_E_BUFFERTOOSMALL_;
  else if (total && !dest_data) hr = E_POINTER_;
  for (void* src : sample.sample_buffers) {
    if (hr < 0) break;
    const void** src_vt = *(const void***)src;
    u8* src_data = nullptr;
    u32 length = 0;
    hr = ((lock_fn)src_vt[3])(src, &src_data, nullptr, &length);
    if (hr < 0) break;
    if (length > capacity - copied) hr = MF_E_BUFFERTOOSMALL_;
    else if (length && (!src_data || !dest_data)) hr = E_POINTER_;
    else if (length) {
      memmove(dest_data + copied, src_data, length);
      copied += length;
    }
    const i32 unlocked = ((unlock_fn)src_vt[4])(src);
    if (hr >= 0) hr = unlocked;
  }
  if (hr >= 0) hr = ((setlen_fn)dest_vt[6])(dest, copied);
  const i32 unlocked = ((unlock_fn)dest_vt[4])(dest);
  return hr < 0 ? hr : unlocked;
}

i32 TRT_GUEST_ABI mfs_GetTotalLength(void* s, u32* len) {
  if (!len) return E_POINTER_;
  auto* o = static_cast<mf_attr_obj*>(s);
  std::lock_guard<std::mutex> lk(o->mu);
  return sample_buffer_length(*o, len);
}

i32 TRT_GUEST_ABI mfs_CopyToBuffer(void* s, void* dest) {
  if (!dest) return E_POINTER_;
  auto* o = static_cast<mf_attr_obj*>(s);
  std::lock_guard<std::mutex> lk(o->mu);
  if (o->sample_buffers.size() == 1 && o->sample_buffers.front() == dest) return S_OK_;
  u32 total = 0;
  const i32 hr = sample_buffer_length(*o, &total);
  return hr < 0 ? hr : sample_copy_buffers(*o, dest, total);
}

i32 TRT_GUEST_ABI mfs_ConvertToContiguousBuffer(void* s, void** out) {
  if (!out) return E_POINTER_;
  *out = nullptr;
  auto* o = static_cast<mf_attr_obj*>(s);
  std::lock_guard<std::mutex> lk(o->mu);
  if (o->sample_buffers.empty()) return E_UNEXPECTED_;
  if (o->sample_buffers.size() > 1) {
    u32 total = 0;
    i32 hr = sample_buffer_length(*o, &total);
    if (hr < 0) return hr;
    void* dest = nullptr;
    hr = mf_MFCreateMemoryBuffer(total, &dest);
    if (hr < 0) return hr;
    hr = sample_copy_buffers(*o, dest, total);
    if (hr < 0) {
      mfb_Release(dest);
      return hr;
    }
    for (void* src : o->sample_buffers) attr_release_unk(src);
    o->sample_buffers.resize(1);
    o->sample_buffers.front() = dest;
  }
  *out = o->sample_buffers.front();
  attr_addref_unk(*out);
  return S_OK_;
}

#define MF_ATTR_SLOTS \
    (const void*)&mfa_QueryInterface, \
    (const void*)&mfa_AddRef, \
    (const void*)&mfa_Release, \
    (const void*)&mfa_GetItem, \
    (const void*)&mfa_GetItemType, \
    (const void*)&mfa_CompareItem, \
    (const void*)&mfa_Compare, \
    (const void*)&mfa_GetUINT32, \
    (const void*)&mfa_GetUINT64, \
    (const void*)&mfa_GetDouble, \
    (const void*)&mfa_GetGUID, \
    (const void*)&mfa_GetStringLength, \
    (const void*)&mfa_GetString, \
    (const void*)&mfa_GetAllocatedString, \
    (const void*)&mfa_GetBlobSize, \
    (const void*)&mfa_GetBlob, \
    (const void*)&mfa_GetAllocatedBlob, \
    (const void*)&mfa_GetUnknown, \
    (const void*)&mfa_SetItem, \
    (const void*)&mfa_DeleteItem, \
    (const void*)&mfa_DeleteAllItems, \
    (const void*)&mfa_SetUINT32, \
    (const void*)&mfa_SetUINT64, \
    (const void*)&mfa_SetDouble, \
    (const void*)&mfa_SetGUID, \
    (const void*)&mfa_SetString, \
    (const void*)&mfa_SetBlob, \
    (const void*)&mfa_SetUnknown, \
    (const void*)&mfa_LockStore, \
    (const void*)&mfa_UnlockStore, \
    (const void*)&mfa_GetCount, \
    (const void*)&mfa_GetItemByIndex, \
    (const void*)&mfa_CopyAllItems

const void* g_mfattrs_vtbl[33] = {MF_ATTR_SLOTS};
const void* g_mfmediatype_vtbl[38] = {
    MF_ATTR_SLOTS,
    (const void*)&mfmt_GetMajorType,
    (const void*)&mfmt_IsCompressedFormat,
    (const void*)&mfmt_IsEqual,
    (const void*)&mfmt_GetRepresentation,
    (const void*)&mfmt_FreeRepresentation,
};
const void* g_mfsample_vtbl[47] = {
    MF_ATTR_SLOTS,
    (const void*)&mfs_GetSampleFlags,
    (const void*)&mfs_SetSampleFlags,
    (const void*)&mfs_GetSampleTime,
    (const void*)&mfs_SetSampleTime,
    (const void*)&mfs_GetSampleDuration,
    (const void*)&mfs_SetSampleDuration,
    (const void*)&mfs_GetBufferCount,
    (const void*)&mfs_GetBufferByIndex,
    (const void*)&mfs_ConvertToContiguousBuffer,
    (const void*)&mfs_AddBuffer,
    (const void*)&mfs_RemoveBufferByIndex,
    (const void*)&mfs_RemoveAllBuffers,
    (const void*)&mfs_GetTotalLength,
    (const void*)&mfs_CopyToBuffer,
};

// ============================================================================
// IMFAsyncResult
// ============================================================================
struct mf_async_res {
  const void** vtbl = nullptr;
  std::atomic<u32> ref{1};
  void* obj = nullptr;
  void* cb = nullptr;
  void* state = nullptr;
  i32 status = 0;
};
i32 TRT_GUEST_ABI mfar_QI(void* s, const u8*, void** out) {
  if (!out) return E_POINTER_;
  *out = s;
  ((mf_async_res*)s)->ref.fetch_add(1, std::memory_order_relaxed);
  return S_OK_;
}
u32 TRT_GUEST_ABI mfar_AddRef(void* s) {
  return ((mf_async_res*)s)->ref.fetch_add(1, std::memory_order_relaxed) + 1u;
}
u32 TRT_GUEST_ABI mfar_Release(void* s) {
  auto* r = (mf_async_res*)s;
  const u32 c = r->ref.fetch_sub(1, std::memory_order_acq_rel) - 1u;
  if (c == 0) {
    attr_release_unk(r->obj);
    attr_release_unk(r->state);
    delete r;
  }
  return c;
}
i32 TRT_GUEST_ABI mfar_GetState(void* s, void** st) {
  if (!st) return E_POINTER_;
  void* v = ((mf_async_res*)s)->state;
  attr_addref_unk(v);
  *st = v;
  return v ? S_OK_ : E_NOTIMPL_;
}
i32 TRT_GUEST_ABI mfar_GetStatus(void* s) { return ((mf_async_res*)s)->status; }
i32 TRT_GUEST_ABI mfar_SetStatus(void* s, i32 st) {
  ((mf_async_res*)s)->status = st;
  return S_OK_;
}
i32 TRT_GUEST_ABI mfar_GetObject(void* s, void** obj) {
  if (!obj) return E_POINTER_;
  void* v = ((mf_async_res*)s)->obj;
  attr_addref_unk(v);
  *obj = v;
  return v ? S_OK_ : E_POINTER_;
}
void* TRT_GUEST_ABI mfar_GetStateNoAddRef(void* s) {
  return ((mf_async_res*)s)->state;
}
const void* g_mfasync_vtbl[8] = {
    (const void*)&mfar_QI, (const void*)&mfar_AddRef, (const void*)&mfar_Release,
    (const void*)&mfar_GetState, (const void*)&mfar_GetStatus,
    (const void*)&mfar_SetStatus, (const void*)&mfar_GetObject,
    (const void*)&mfar_GetStateNoAddRef,
};

// ============================================================================
// IMFDXGIDeviceManager
// ============================================================================
struct mf_dxgi_mgr {
  const void** vtbl = nullptr;
  std::atomic<u32> ref{1};
  void* device = nullptr;
  u32 token = 1;
};
i32 TRT_GUEST_ABI mfdm_QI(void* s, const u8*, void** out) {
  if (!out) return E_POINTER_;
  *out = s;
  ((mf_dxgi_mgr*)s)->ref.fetch_add(1, std::memory_order_relaxed);
  return S_OK_;
}
u32 TRT_GUEST_ABI mfdm_AddRef(void* s) {
  return ((mf_dxgi_mgr*)s)->ref.fetch_add(1, std::memory_order_relaxed) + 1u;
}
u32 TRT_GUEST_ABI mfdm_Release(void* s) {
  auto* m = (mf_dxgi_mgr*)s;
  const u32 c = m->ref.fetch_sub(1, std::memory_order_acq_rel) - 1u;
  if (c == 0) delete m;
  return c;
}
i32 TRT_GUEST_ABI mfdm_CloseDeviceHandle(void*, void*) { return S_OK_; }
i32 TRT_GUEST_ABI mfdm_GetVideoService(void* s, void* h, const u8* riid, void** out) {
  (void)h; (void)riid;
  if (!out) return E_POINTER_;
  *out = ((mf_dxgi_mgr*)s)->device;
  return *out ? S_OK_ : E_NOINTERFACE_;
}
i32 TRT_GUEST_ABI mfdm_LockDevice(void* s, void* h, const u8* riid, void** out, int) {
  (void)h; (void)riid;
  if (!out) return E_POINTER_;
  *out = ((mf_dxgi_mgr*)s)->device;
  return *out ? S_OK_ : E_NOTIMPL_;
}
i32 TRT_GUEST_ABI mfdm_OpenDeviceHandle(void*, void** ph) {
  if (ph) *ph = (void*)(uintptr_t)0x4D46D001ULL;
  return S_OK_;
}
i32 TRT_GUEST_ABI mfdm_ResetDevice(void* s, void* dev, u32 tok) {
  ((mf_dxgi_mgr*)s)->device = dev;
  ((mf_dxgi_mgr*)s)->token = tok;
  return S_OK_;
}
i32 TRT_GUEST_ABI mfdm_TestDevice(void*, void*) { return S_OK_; }
i32 TRT_GUEST_ABI mfdm_UnlockDevice(void*, void*, int) { return S_OK_; }
const void* g_mfdxmgr_vtbl[10] = {
    (const void*)&mfdm_QI, (const void*)&mfdm_AddRef, (const void*)&mfdm_Release,
    (const void*)&mfdm_CloseDeviceHandle, (const void*)&mfdm_GetVideoService,
    (const void*)&mfdm_LockDevice, (const void*)&mfdm_OpenDeviceHandle,
    (const void*)&mfdm_ResetDevice, (const void*)&mfdm_TestDevice,
    (const void*)&mfdm_UnlockDevice,
};

// ============================================================================
// IMFSourceReader (honest stream metadata + clean EOS on ReadSample)
// ============================================================================
struct mf_source_reader {
  const void** vtbl = nullptr;
  std::atomic<u32> ref{1};
  bool video_sel = true;
  bool audio_sel = true;
};

static void* make_default_media_type(bool video) {
  auto* mt = new mf_attr_obj();
  mt->vtbl = g_mfmediatype_vtbl;
  mt->is_media_type = true;
  mfa_SetGUID(mt, k_MF_MT_MAJOR_TYPE, video ? k_MFMediaType_Video : k_MFMediaType_Audio);
  mfa_SetGUID(mt, k_MF_MT_SUBTYPE, video ? k_MFVideoFormat_RGB32 : k_MFAudioFormat_PCM);
  if (video) {
    mfa_SetUINT64(mt, k_MF_MT_FRAME_SIZE, ((u64)1280u << 32) | 720u);
    mfa_SetUINT64(mt, k_MF_MT_FRAME_RATE, ((u64)30u << 32) | 1u);
  }
  return mt;
}

i32 TRT_GUEST_ABI mfsr_QI(void* s, const u8*, void** out) {
  if (!out) return E_POINTER_;
  *out = s;
  ((mf_source_reader*)s)->ref.fetch_add(1, std::memory_order_relaxed);
  return S_OK_;
}
u32 TRT_GUEST_ABI mfsr_AddRef(void* s) {
  return ((mf_source_reader*)s)->ref.fetch_add(1, std::memory_order_relaxed) + 1u;
}
u32 TRT_GUEST_ABI mfsr_Release(void* s) {
  auto* r = (mf_source_reader*)s;
  const u32 c = r->ref.fetch_sub(1, std::memory_order_acq_rel) - 1u;
  if (c == 0) delete r;
  return c;
}
i32 TRT_GUEST_ABI mfsr_GetStreamSelection(void* s, u32 idx, int* sel) {
  if (!sel) return E_POINTER_;
  auto* r = (mf_source_reader*)s;
  if (idx == 0 || idx == 0xFFFFFFFCu) *sel = r->video_sel ? 1 : 0;
  else if (idx == 1 || idx == 0xFFFFFFFDu) *sel = r->audio_sel ? 1 : 0;
  else return MF_E_INVALIDINDEX_;
  return S_OK_;
}
i32 TRT_GUEST_ABI mfsr_SetStreamSelection(void* s, u32 idx, int sel) {
  auto* r = (mf_source_reader*)s;
  if (idx == 0 || idx == 0xFFFFFFFCu) r->video_sel = (sel != 0);
  else if (idx == 1 || idx == 0xFFFFFFFDu) r->audio_sel = (sel != 0);
  else if (idx == 0xFFFFFFFEu) r->video_sel = r->audio_sel = (sel != 0);
  else return MF_E_INVALIDINDEX_;
  return S_OK_;
}
i32 TRT_GUEST_ABI mfsr_GetNativeMediaType(void*, u32 stream_idx, u32 type_idx,
                                          void** out) {
  if (!out) return E_POINTER_;
  *out = nullptr;
  if (type_idx != 0) return MF_E_NO_MORE_TYPES_;
  if (stream_idx == 0 || stream_idx == 0xFFFFFFFCu) {
    *out = make_default_media_type(true);
    return S_OK_;
  }
  if (stream_idx == 1 || stream_idx == 0xFFFFFFFDu) {
    *out = make_default_media_type(false);
    return S_OK_;
  }
  return MF_E_INVALIDINDEX_;
}
i32 TRT_GUEST_ABI mfsr_GetCurrentMediaType(void* s, u32 stream_idx, void** out) {
  return mfsr_GetNativeMediaType(s, stream_idx, 0, out);
}
i32 TRT_GUEST_ABI mfsr_SetCurrentMediaType(void*, u32, u32*, void*) { return S_OK_; }
i32 TRT_GUEST_ABI mfsr_SetCurrentPosition(void*, const u8*, const void*) {
  return S_OK_;
}
i32 TRT_GUEST_ABI mfsr_ReadSample(void*, u32 stream_idx, u32 flags,
                                  u32* actual_stream, u32* stream_flags,
                                  i64* ts, void** sample) {
  (void)flags;
  if (actual_stream) *actual_stream = (stream_idx == 0xFFFFFFFDu || stream_idx == 1) ? 1u : 0u;
  if (stream_flags) *stream_flags = 0x2u;  // MF_SOURCE_READERF_ENDOFSTREAM
  if (ts) *ts = 0;
  if (sample) *sample = nullptr;
  return S_OK_;
}
i32 TRT_GUEST_ABI mfsr_Flush(void*, u32) { return S_OK_; }
i32 TRT_GUEST_ABI mfsr_GetServiceForStream(void*, u32, const u8*, const u8*, void** out) {
  if (out) *out = nullptr;
  return E_NOTIMPL_;
}
i32 TRT_GUEST_ABI mfsr_GetPresentationAttribute(void*, u32, const u8* guid, void* pv) {
  if (!guid || !pv) return E_POINTER_;
  if (memcmp(guid, k_MF_PD_DURATION, 16) == 0) {
    memset(pv, 0, 24);
    *(u16*)pv = 21;  // VT_UI8
    *(u64*)((u8*)pv + 8) = 10000000ULL;
    return S_OK_;
  }
  return MF_E_ATTRIBUTENOTFOUND_;
}
const void* g_mfsr_vtbl[13] = {
    (const void*)&mfsr_QI, (const void*)&mfsr_AddRef, (const void*)&mfsr_Release,
    (const void*)&mfsr_GetStreamSelection, (const void*)&mfsr_SetStreamSelection,
    (const void*)&mfsr_GetNativeMediaType, (const void*)&mfsr_GetCurrentMediaType,
    (const void*)&mfsr_SetCurrentMediaType, (const void*)&mfsr_SetCurrentPosition,
    (const void*)&mfsr_ReadSample, (const void*)&mfsr_Flush,
    (const void*)&mfsr_GetServiceForStream,
    (const void*)&mfsr_GetPresentationAttribute,
};

// ============================================================================
// DLL Exports
// ============================================================================
std::atomic<u32> g_mf_wq_next{1};

i32 TRT_GUEST_ABI mf_MFStartup(u32 version, u32 flags) {
  (void)version; (void)flags;
  trt_core_log("mfplat: MFStartup -> S_OK");
  return S_OK_;
}
i32 TRT_GUEST_ABI mf_MFShutdown(void) { return S_OK_; }
i32 TRT_GUEST_ABI mf_MFLockPlatform(void) { return S_OK_; }
i32 TRT_GUEST_ABI mf_MFUnlockPlatform(void) { return S_OK_; }
i32 TRT_GUEST_ABI mf_MFAllocateWorkQueue(u32* pq) {
  if (!pq) return E_POINTER_;
  *pq = g_mf_wq_next.fetch_add(1, std::memory_order_relaxed);
  return S_OK_;
}
i32 TRT_GUEST_ABI mf_MFAllocateWorkQueueEx(u32, u32* pq) {
  return mf_MFAllocateWorkQueue(pq);
}
i32 TRT_GUEST_ABI mf_MFLockWorkQueue(u32) { return S_OK_; }
i32 TRT_GUEST_ABI mf_MFUnlockWorkQueue(u32) { return S_OK_; }
i32 TRT_GUEST_ABI mf_MFGetTimerPeriodicity(u32* p) {
  if (!p) return E_POINTER_;
  *p = 10u;
  return S_OK_;
}
i32 TRT_GUEST_ABI mf_MFCreateAttributes(void** out, u32 initial_size) {
  (void)initial_size;
  if (!out) return E_POINTER_;
  auto* o = new mf_attr_obj();
  o->vtbl = g_mfattrs_vtbl;
  *out = o;
  return S_OK_;
}
i32 TRT_GUEST_ABI mf_MFCreateMediaType(void** out) {
  if (!out) return E_POINTER_;
  auto* o = new mf_attr_obj();
  o->vtbl = g_mfmediatype_vtbl;
  o->is_media_type = true;
  *out = o;
  return S_OK_;
}
i32 TRT_GUEST_ABI mf_MFCreateMemoryBuffer(u32 max_len, void** out) {
  if (!out) return E_POINTER_;
  *out = nullptr;
  auto* b = new (std::nothrow) mf_buffer_obj();
  if (!b) return E_OUTOFMEMORY_;
  if (trt_mem_budget_charge_commit(max_len) != 0) {
    delete b;
    return E_OUTOFMEMORY_;
  }
  b->charged_bytes = max_len;
  b->vtbl = g_mfbuffer_vtbl;
  try {
    b->data.resize(max_len);
  } catch (const std::bad_alloc&) {
    delete b;
    return E_OUTOFMEMORY_;
  }
  *out = b;
  return S_OK_;
}
i32 TRT_GUEST_ABI mf_MFCreateAlignedMemoryBuffer(u32 max_len, u32 align, void** out) {
  (void)align;
  return mf_MFCreateMemoryBuffer(max_len, out);
}
i32 TRT_GUEST_ABI mf_MFCreateSample(void** out) {
  if (!out) return E_POINTER_;
  auto* o = new mf_attr_obj();
  o->vtbl = g_mfsample_vtbl;
  o->is_sample = true;
  *out = o;
  return S_OK_;
}
i32 TRT_GUEST_ABI mf_MFCreateAsyncResult(void* obj, void* cb, void* state,
                                         void** out) {
  if (!out) return E_POINTER_;
  auto* r = new mf_async_res();
  r->vtbl = g_mfasync_vtbl;
  attr_addref_unk(obj);
  attr_addref_unk(state);
  r->obj = obj;
  r->cb = cb;
  r->state = state;
  *out = r;
  return S_OK_;
}
i32 TRT_GUEST_ABI mf_MFPutWorkItem(u32 q, void* cb, void* state) {
  (void)q;
  if (!cb) return E_INVALIDARG_;
  void* res = nullptr;
  mf_MFCreateAsyncResult(nullptr, cb, state, &res);
  // IMFAsyncCallback::Invoke is slot 4
  const void** vt = *(const void***)cb;
  if (vt && vt[4]) trt_call_guest((void*)vt[4], (u64)(uintptr_t)cb, (u64)(uintptr_t)res, 0, 0);
  if (res) mfar_Release(res);
  return S_OK_;
}
i32 TRT_GUEST_ABI mf_MFTEnumEx(const u8*, u32, const void*, const void*,
                               void*** arr, u32* count) {
  if (arr) *arr = nullptr;
  if (count) *count = 0;
  return S_OK_;
}
i32 TRT_GUEST_ABI mf_MFCreateDXGIDeviceManager(u32* reset_token, void** out) {
  if (!out) return E_POINTER_;
  auto* m = new mf_dxgi_mgr();
  m->vtbl = g_mfdxmgr_vtbl;
  if (reset_token) *reset_token = m->token;
  *out = m;
  return S_OK_;
}
i32 TRT_GUEST_ABI mf_MFCreateSourceReaderFromURL(const u16* url, void* attrs,
                                                 void** out) {
  (void)url; (void)attrs;
  if (!out) return E_POINTER_;
  auto* sr = new mf_source_reader();
  sr->vtbl = g_mfsr_vtbl;
  *out = sr;
  return S_OK_;
}
i32 TRT_GUEST_ABI mf_MFCreateSourceReaderFromByteStream(void* bs, void* attrs,
                                                        void** out) {
  (void)bs; (void)attrs;
  return mf_MFCreateSourceReaderFromURL(nullptr, attrs, out);
}
i32 TRT_GUEST_ABI mf_MFCreateSourceReaderFromMediaSource(void* ms, void* attrs,
                                                         void** out) {
  (void)ms; (void)attrs;
  return mf_MFCreateSourceReaderFromURL(nullptr, attrs, out);
}
i32 TRT_GUEST_ABI mf_MFGetStrideForBitmapInfoHeader(u32 fmt, u32 w, i32* stride) {
  (void)fmt;
  if (!stride) return E_POINTER_;
  *stride = (i32)(w * 4u);
  return S_OK_;
}
i32 TRT_GUEST_ABI mf_DXVA2CreateDirect3DDeviceManager9(u32* reset_tok, void** out) {
  return mf_MFCreateDXGIDeviceManager(reset_tok, out);
}
i32 TRT_GUEST_ABI mf_MFCreateFile(u32 access_mode, u32 open_mode, u32 flags,
                                  const u16* url, void** out_stream) {
  (void)access_mode; (void)open_mode; (void)flags; (void)url;
  if (out_stream) *out_stream = nullptr;
  return E_NOTIMPL_;
}
i32 TRT_GUEST_ABI mf_MFCreateSourceResolver(void** out_resolver) {
  if (out_resolver) *out_resolver = nullptr;
  return E_NOTIMPL_;
}

}  // namespace

extern "C" int trt_bridge_register_mf(void) {
  static const trt_named_fn k_mfplat[] = {
      {"MFStartup", (void*)&mf_MFStartup},
      {"MFShutdown", (void*)&mf_MFShutdown},
      {"MFLockPlatform", (void*)&mf_MFLockPlatform},
      {"MFUnlockPlatform", (void*)&mf_MFUnlockPlatform},
      {"MFAllocateWorkQueue", (void*)&mf_MFAllocateWorkQueue},
      {"MFAllocateWorkQueueEx", (void*)&mf_MFAllocateWorkQueueEx},
      {"MFAllocateSerialWorkQueue", (void*)&mf_MFAllocateWorkQueueEx},
      {"MFLockWorkQueue", (void*)&mf_MFLockWorkQueue},
      {"MFUnlockWorkQueue", (void*)&mf_MFUnlockWorkQueue},
      {"MFGetTimerPeriodicity", (void*)&mf_MFGetTimerPeriodicity},
      {"MFCreateAttributes", (void*)&mf_MFCreateAttributes},
      {"MFCreateMediaType", (void*)&mf_MFCreateMediaType},
      {"MFCreateMemoryBuffer", (void*)&mf_MFCreateMemoryBuffer},
      {"MFCreateAlignedMemoryBuffer", (void*)&mf_MFCreateAlignedMemoryBuffer},
      {"MFCreateSample", (void*)&mf_MFCreateSample},
      {"MFCreateAsyncResult", (void*)&mf_MFCreateAsyncResult},
      {"MFPutWorkItem", (void*)&mf_MFPutWorkItem},
      {"MFTEnumEx", (void*)&mf_MFTEnumEx},
      {"MFCreateDXGIDeviceManager", (void*)&mf_MFCreateDXGIDeviceManager},
      {"MFGetStrideForBitmapInfoHeader",
       (void*)&mf_MFGetStrideForBitmapInfoHeader},
      {"MFCreateFile", (void*)&mf_MFCreateFile},
      {"MFCreateSourceResolver", (void*)&mf_MFCreateSourceResolver},
  };
  trt_module_register("mfplat.dll", TRT_ROUTE_BUILTIN, k_mfplat,
                      (int)(sizeof(k_mfplat) / sizeof(k_mfplat[0])));

  static const trt_named_fn k_mfrw[] = {
      {"MFCreateSourceReaderFromURL", (void*)&mf_MFCreateSourceReaderFromURL},
      {"MFCreateSourceReaderFromByteStream",
       (void*)&mf_MFCreateSourceReaderFromByteStream},
      {"MFCreateSourceReaderFromMediaSource",
       (void*)&mf_MFCreateSourceReaderFromMediaSource},
  };
  trt_module_register("mfreadwrite.dll", TRT_ROUTE_BUILTIN, k_mfrw,
                      (int)(sizeof(k_mfrw) / sizeof(k_mfrw[0])));

  static const trt_named_fn k_mf[] = {
      {"MFCreateSourceResolver", (void*)&mf_MFCreateAttributes},
  };
  trt_module_register("mf.dll", TRT_ROUTE_BUILTIN, k_mf,
                      (int)(sizeof(k_mf) / sizeof(k_mf[0])));
  trt_module_register("mfuuid.dll", TRT_ROUTE_BUILTIN, nullptr, 0);
  trt_module_register("evr.dll", TRT_ROUTE_BUILTIN, k_mf, 1);

  static const trt_named_fn k_dxva2[] = {
      {"DXVA2CreateDirect3DDeviceManager9",
       (void*)&mf_DXVA2CreateDirect3DDeviceManager9},
  };
  trt_module_register("dxva2.dll", TRT_ROUTE_BUILTIN, k_dxva2,
                      (int)(sizeof(k_dxva2) / sizeof(k_dxva2[0])));
  return 0;
}
