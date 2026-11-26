// tiramisu/src/trt/bridge_version.cpp
// version.dll / api-ms-win-core-version-l1-1-* bridge.
#include "bridge_version.h"
#include "pe.h"
#include "thunk.h"
#include "trt.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

namespace verlog { static void line(const char* s) { trt_core_log(s); } }
#define log_line(s_) verlog::line(s_)

extern "C" {
void TRT_STDCALL trt_SetLastError(u32 e);
u32  TRT_STDCALL trt_GetLastError(void);
}

namespace {
// ---- file read through the guest VFS --------------------------------------
static bool read_file_bytes(const u16* guest_path, std::vector<unsigned char>* out) {
  if (!guest_path || !out) return false;
  char host[1024];
  if (trt_vfs_resolve_utf16(guest_path, host, sizeof(host)) != 0) return false;
  FILE* f = std::fopen(host, "rb");
  if (!f) return false;
  out->clear();
  unsigned char tmp[4096];
  size_t got;
  while ((got = std::fread(tmp, 1, sizeof(tmp), f)) > 0) {
    out->insert(out->end(), tmp, tmp + got);
    if (out->size() > 64u * 1024u * 1024u) { std::fclose(f); return false; }
  }
  std::fclose(f);
  return !out->empty();
}

// ---- PE resource extraction (RT_VERSION) from file bytes --------------------
static u16 rd16(const unsigned char* p) { u16 v; std::memcpy(&v, p, 2); return v; }
static u32 rd32(const unsigned char* p) { u32 v; std::memcpy(&v, p, 4); return v; }

// find section containing an RVA; return file offset of the RVA, or 0 on fail
static u64 rva_to_offset(const unsigned char* base, size_t size, u32 rva) {
  if (!base || rd16(base) != 0x5A4D) return 0;
  u32 lfanew = rd32(base + 0x3C);
  if (lfanew + 248 > size) return 0;
  if (rd32(base + lfanew) != 0x00004550u) return 0;
  const unsigned char* opt = base + lfanew + 24;
  u16 magic = rd16(opt);
  if (magic != 0x020Bu && magic != 0x010Bu) return 0;
  u32 num_sections = rd16(base + lfanew + 6);
  u16 size_opt = rd16(base + lfanew + 20);
  const unsigned char* sec = base + lfanew + 24 + size_opt;
  for (u32 i = 0; i < num_sections; ++i) {
    if ((size_t)(sec - base) + 40 > size) return 0;
    u32 va = rd32(sec + 12);
    u32 vsz = rd32(sec + 8);
    u32 raw = rd32(sec + 20);
    u32 rsz = rd32(sec + 16);
    if (rva >= va && rva < va + (vsz ? vsz : rsz)) {
      u64 off = (u64)raw + (rva - va);
      if (off + 1 > size) return 0;
      return off;
    }
    sec += 40;
  }
  // maybe rva is in a header (raw == rva for some)
  if (rva < size) return rva;
  return 0;
}

// extract RT_VERSION (type 16) resource data (first name/language) from file
static bool extract_version_resource(const std::vector<unsigned char>& file,
                                    std::vector<unsigned char>* out) {
  if (!out) return false;
  out->clear();
  const unsigned char* base = file.data();
  size_t size = file.size();
  if (size < 64 || rd16(base) != 0x5A4D) return false;
  u32 lfanew = rd32(base + 0x3C);
  if ((size_t)lfanew + 248 > size || rd32(base + lfanew) != 0x00004550u) return false;
  const unsigned char* opt = base + lfanew + 24;
  u16 magic = rd16(opt);
  if (magic != 0x020Bu) return false; // PE32+ only
  const unsigned char* dirs = opt + 112; // data directories start
  if ((size_t)(dirs - base) + 16 > size) return false;
  u32 res_rva = rd32(dirs + 2 * 8);
  u32 res_sz = rd32(dirs + 2 * 8 + 4);
  if (!res_rva || !res_sz) return false;
  u64 res_off = rva_to_offset(base, size, res_rva);
  if (!res_off || res_off + 16 > size) return false;

  // resource dir header: 16 bytes then (named+ided) 8-byte entries. Child
  // offsets are SECTION-relative to the resource directory.
  auto dir_count = [&](u64 off, u16* total) {
    if (off + 16 > size) return false;
    *total = (u16)(rd16(base + off + 12) + rd16(base + off + 14));
    return true;
  };
  auto first_child = [&](u64 off, u32* child_rva) {
    if (off + 16 + 8 > size) return false;
    *child_rva = res_rva + (rd32(base + off + 16 + 4) & 0x7FFFFFFFu);
    return true;
  };

  u16 type_count = 0;
  if (!dir_count(res_off, &type_count)) return false;
  u32 name_dir_rva = 0;
  for (u16 i = 0; i < type_count; ++i) {
    const unsigned char* e = base + res_off + 16 + (size_t)i * 8;
    if ((size_t)(e - base) + 8 > size) return false;
    u32 id = rd32(e);
    if ((id & 0x80000000u) == 0 && (id & 0xFFFFu) == 16) {
      name_dir_rva = res_rva + (rd32(e + 4) & 0x7FFFFFFFu);
      break;
    }
  }
  if (!name_dir_rva) return false;
  u64 name_dir_off = rva_to_offset(base, size, name_dir_rva);
  if (!name_dir_off) return false;
  u16 name_count = 0;
  if (!dir_count(name_dir_off, &name_count) || name_count == 0) return false;
  u32 lang_dir_rva = 0;
  if (!first_child(name_dir_off, &lang_dir_rva)) return false;
  u64 lang_dir_off = rva_to_offset(base, size, lang_dir_rva);
  if (!lang_dir_off) return false;
  u16 lang_count = 0;
  if (!dir_count(lang_dir_off, &lang_count) || lang_count == 0) return false;
  u32 data_entry_rva = 0;
  if (!first_child(lang_dir_off, &data_entry_rva)) return false;
  u64 data_entry_off = rva_to_offset(base, size, data_entry_rva);
  if (!data_entry_off || data_entry_off + 16 > size) return false;
  u32 data_rva = rd32(base + data_entry_off);
  u32 data_sz = rd32(base + data_entry_off + 4);
  if (!data_rva || !data_sz) return false;
  u64 data_off = rva_to_offset(base, size, data_rva);
  if (!data_off || data_off + data_sz > size) return false;
  out->assign(base + data_off, base + data_off + data_sz);
  return true;
}

// ---- version block parsing helpers -----------------------------------------
struct VBlock { const unsigned char* base; size_t avail; u16 total; u16 vlen; u16 type; std::u16string key; size_t valoff; size_t valbytes; size_t childoff; };

static bool parse_block(const unsigned char* bytes, size_t avail, VBlock* r) {
  if (!bytes || !r || avail < 6) return false;
  VBlock b;
  b.base = bytes; b.avail = avail;
  b.total = rd16(bytes); b.vlen = rd16(bytes + 2); b.type = rd16(bytes + 4);
  if (b.total < 6 || b.total > avail) return false;
  size_t cur = 6;
  while (true) {
    if (cur + 2 > b.total) return false;
    u16 ch = rd16(bytes + cur);
    cur += 2;
    if (ch == 0) break;
    b.key.push_back((char16_t)ch);
  }
  b.valoff = (cur + 3) & ~size_t(3);
  b.valbytes = (b.type == 1) ? (size_t)b.vlen * 2 : (size_t)b.vlen;
  if (b.valoff > b.total || b.valbytes > b.total - b.valoff) return false;
  b.childoff = (b.valoff + b.valbytes + 3) & ~size_t(3);
  if (b.childoff > b.total) b.childoff = b.total;
  *r = b;
  return true;
}

static bool find_child(const VBlock& parent, const std::u16string& key, VBlock* child) {
  size_t cur = parent.childoff;
  while (cur + 6 <= parent.total) {
    VBlock c;
    if (!parse_block(parent.base + cur, parent.total - cur, &c) || c.total == 0) return false;
    if (c.key == key) { *child = c; child->base = parent.base + cur; child->avail = parent.total - cur; return true; }
    cur = (cur + c.total + 3) & ~size_t(3);
  }
  return false;
}

static std::vector<std::u16string> split_path(const u16* p) {
  std::vector<std::u16string> parts;
  if (!p) return parts;
  const u16* c = p;
  while (*c) {
    while (*c == u'\\' || *c == u'/') ++c;
    const u16* s = c;
    while (*c && *c != u'\\' && *c != u'/') ++c;
    if (c != s) parts.emplace_back(s, c);
  }
  return parts;
}

// VerQueryValue: returns pointer into block + length (in chars or bytes)
static bool query_value(const void* data, const u16* path, void** value, u32* length) {
  if (!data || !path || !value || !length) return false;
  *value = nullptr; *length = 0;
  const unsigned char* bytes = (const unsigned char*)data;
  u16 rootlen = rd16(bytes);
  VBlock cur;
  if (!parse_block(bytes, rootlen, &cur) || cur.key != u"VS_VERSION_INFO") return false;
  for (auto& part : split_path(path)) {
    VBlock nxt;
    if (!find_child(cur, part, &nxt)) return false;
    cur = nxt;
  }
  *value = (void*)(cur.base + cur.valoff);
  *length = (u32)cur.vlen;
  return true;
}

static void set_err(u32 e) { trt_SetLastError(e); }

// read version resource from a guest file path into a vector
static bool load_version_resource(const u16* guest_path, std::vector<unsigned char>* res) {
  std::vector<unsigned char> file;
  if (!read_file_bytes(guest_path, &file)) { set_err(2); return false; }
  if (!extract_version_resource(file, res)) { set_err(1813); return false; } // ERROR_RESOURCE_TYPE_NOT_FOUND-ish
  set_err(0);
  return true;
}

// ANSI <-> wide helpers
static bool ansi_to_wide(const char* s, std::u16string* out) {
  if (!s || !out) return false;
  // simple: treat as UTF-8-ish / latin1 -> u16 (codepoint = byte for <256)
  std::u16string r;
  for (const unsigned char* p = (const unsigned char*)s; *p; ++p) r.push_back((char16_t)*p);
  *out = r;
  return true;
}
} // namespace

// ---- exported functions (TRT_STDCALL) ----

extern "C" {

u32 TRT_STDCALL trt_GetFileVersionInfoSizeW(const u16* file, u32* handle) {
  if (handle) *handle = 0;
  std::vector<unsigned char> res;
  if (!load_version_resource(file, &res)) { return 0; }
  set_err(0);
  return (u32)res.size();
}

u32 TRT_STDCALL trt_GetFileVersionInfoSizeA(const char* file, u32* handle) {
  std::u16string w;
  if (!ansi_to_wide(file, &w)) { set_err(1113); return 0; }
  return trt_GetFileVersionInfoSizeW((const u16*)w.c_str(), handle);
}

u32 TRT_STDCALL trt_GetFileVersionInfoSizeExW(u32 flags, const u16* file, u32* handle) {
  if (flags & ~0x7u) { set_err(87); return 0; }
  return trt_GetFileVersionInfoSizeW(file, handle);
}
u32 TRT_STDCALL trt_GetFileVersionInfoSizeExA(u32 flags, const char* file, u32* handle) {
  if (flags & ~0x7u) { set_err(87); return 0; }
  return trt_GetFileVersionInfoSizeA(file, handle);
}

int TRT_STDCALL trt_GetFileVersionInfoW(const u16* file, u32 handle, u32 len, void* data) {
  (void)handle;
  if (!data) { set_err(87); return 0; }
  std::vector<unsigned char> res;
  if (!load_version_resource(file, &res)) return 0;
  if (len < res.size()) { set_err(122); return 0; }
  std::memcpy(data, res.data(), res.size());
  set_err(0);
  return 1;
}
int TRT_STDCALL trt_GetFileVersionInfoA(const char* file, u32 handle, u32 len, void* data) {
  std::u16string w;
  if (!ansi_to_wide(file, &w)) { set_err(1113); return 0; }
  return trt_GetFileVersionInfoW((const u16*)w.c_str(), handle, len, data);
}
int TRT_STDCALL trt_GetFileVersionInfoExW(u32 flags, const u16* file, u32 handle, u32 len, void* data) {
  if (flags & ~0x7u) { set_err(87); return 0; }
  return trt_GetFileVersionInfoW(file, handle, len, data);
}
int TRT_STDCALL trt_GetFileVersionInfoExA(u32 flags, const char* file, u32 handle, u32 len, void* data) {
  if (flags & ~0x7u) { set_err(87); return 0; }
  return trt_GetFileVersionInfoA(file, handle, len, data);
}

int TRT_STDCALL trt_VerQueryValueW(const void* block, const u16* sub, void** value, u32* len) {
  u32 l = 0;
  void* v = nullptr;
  if (!query_value(block, sub, &v, &l)) { set_err(1814); return 0; }
  if (value) *value = v;
  if (len) *len = l;
  set_err(0);
  return 1;
}

// thread-local ANSI conversion buffer for VerQueryValueA string results
static thread_local std::vector<char> g_conv;

int TRT_STDCALL trt_VerQueryValueA(const void* block, const char* sub, void** value, u32* len) {
  std::u16string wsub;
  if (!ansi_to_wide(sub, &wsub)) { set_err(1113); return 0; }
  u32 l = 0; void* v = nullptr;
  if (!query_value(block, (const u16*)wsub.c_str(), &v, &l)) { set_err(1814); return 0; }
  // detect string leaf: path has >=3 parts and first is StringFileInfo
  auto parts = split_path((const u16*)wsub.c_str());
  bool isstr = (parts.size() >= 3 && parts[0] == u"StringFileInfo");
  if (isstr && v && l > 0) {
    const u16* src = (const u16*)v;
    // src is a u16 string of length l (chars). Convert to ANSI (latin1-ish)
    g_conv.clear();
    for (u32 i = 0; i < l && src[i]; ++i) g_conv.push_back((char)(src[i] & 0xFF));
    g_conv.push_back(0);
    if (value) *value = g_conv.data();
    if (len) *len = (u32)g_conv.size();
    set_err(0);
    return 1;
  }
  if (value) *value = v;
  if (len) *len = l;
  set_err(0);
  return 1;
}

u32 TRT_STDCALL trt_VerLanguageNameW(u32 lang, u16* buf, u32 count) {
  if (!buf || count == 0) return 0;
  // produce a simple name
  const u16* name = (const u16*)u"English (United States)";
  u32 n = 0; while (name[n]) ++n;
  if (count <= n) return n + 1; // need space for null
  for (u32 i = 0; i <= n; ++i) buf[i] = name[i];
  return n;
}

u32 TRT_STDCALL trt_VerLanguageNameA(u32 lang, char* buf, u32 count) {
  if (!buf || count == 0) return 0;
  const char* name = "English (United States)";
  u32 n = (u32)std::strlen(name);
  if (count <= n) return n + 1;
  std::memcpy(buf, name, n + 1);
  return n;
}

} // extern "C"

// ---- registration ----
extern "C" int trt_bridge_register_version(void) {
  static const trt_named_fn k_fns[] = {
      {"GetFileVersionInfoSizeW", (void*)&trt_GetFileVersionInfoSizeW},
      {"GetFileVersionInfoSizeA", (void*)&trt_GetFileVersionInfoSizeA},
      {"GetFileVersionInfoSizeExW", (void*)&trt_GetFileVersionInfoSizeExW},
      {"GetFileVersionInfoSizeExA", (void*)&trt_GetFileVersionInfoSizeExA},
      {"GetFileVersionInfoW", (void*)&trt_GetFileVersionInfoW},
      {"GetFileVersionInfoA", (void*)&trt_GetFileVersionInfoA},
      {"GetFileVersionInfoExW", (void*)&trt_GetFileVersionInfoExW},
      {"GetFileVersionInfoExA", (void*)&trt_GetFileVersionInfoExA},
      {"VerQueryValueW", (void*)&trt_VerQueryValueW},
      {"VerQueryValueA", (void*)&trt_VerQueryValueA},
      {"VerLanguageNameW", (void*)&trt_VerLanguageNameW},
      {"VerLanguageNameA", (void*)&trt_VerLanguageNameA},
  };
  trt_module_register("version.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  // api-ms-win-core-version l1 aliases
  trt_module_register("api-ms-win-core-version-l1-1-0.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  trt_module_register("api-ms-win-core-version-l1-1-1.dll", TRT_ROUTE_BUILTIN, k_fns,
                      (int)(sizeof(k_fns) / sizeof(k_fns[0])));
  log_line("version: registered version.dll + api-ms-win-core-version");
  return 0;
}
