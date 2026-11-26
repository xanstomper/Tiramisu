// tiramisu/src/trt/bridge_actctx.cpp
// Activation-context bridge. See bridge_actctx.h (attribution there).
//
// Honest-surface rules:
//   * Handles are synthetic tokens; nothing here pretends to be WinSxS. A
//     dependent assembly is *recorded* (identity visible to Query), never
//     faked as loaded.
//   * FindActCtxSection* answers "not found" (ERROR_SXS_KEY_NOT_FOUND) — the
//     truthful result for a runtime without a side-by-side store.
//   * Failures set the real Win32 error code and return FALSE/INVALID_HANDLE.
#include "bridge_actctx.h"
#include "bridge_res.h"
#include "images.h"
#include "thunk.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
void TRT_STDCALL trt_SetLastError(u32 e);
}

namespace {

// ---- Win32 constants ------------------------------------------------------
constexpr u32 kFlagProcArch = 0x01, kFlagLang = 0x02, kFlagAsmDir = 0x04,
              kFlagResName = 0x08, kFlagSetProcessDefault = 0x10,
              kFlagAppName = 0x20, kFlagModule = 0x80;
constexpr u32 kDeactForceEarly = 0x1;
constexpr u32 kQueryUseActive = 0x4, kQueryNoAddRef = 0x8;
constexpr u32 kErrFileNotFound = 2, kErrInvalidHandle = 6, kErrNotSupported = 50,
              kErrInvalidParam = 87, kErrInsufficient = 122,
              kErrSxsCantGen = 14001, kErrSxsKeyNotFound = 14007,
              kErrSxsEarlyDeact = 14009, kErrSxsInvalidDeact = 14010;
constexpr u64 kTokenBase = 0x74000000ull;
const u64 kInvalidHandle = ~0ull;

// ---- parsed manifest ------------------------------------------------------
struct Ident {
  std::string name, version, arch, token, lang, type;
};

struct Manifest {
  std::string xml;
  std::u16string source;
  std::u16string app_dir;
  Ident self;
  std::vector<Ident> deps;
  std::vector<std::string> files;
  std::map<std::string, std::string> settings;  // lowercase local name -> text
  std::vector<std::string> supported_os;        // lowercase "{guid}"
  int run_level = 0;
  bool ui_access = false;
  bool comctl6 = false;
};

struct Ctx {
  Manifest m;
  std::atomic<long> refs{1};
  std::u16string resource_name;
};

std::mutex g_mu;
std::unordered_map<u64, std::shared_ptr<Ctx>> g_ctxs;
std::atomic<u64> g_next_token{kTokenBase};
std::atomic<u64> g_next_cookie{1};
std::atomic<u64> g_process_default{0};

struct Active {
  u64 ctx;
  u64 cookie;
};
thread_local std::vector<Active> t_active;

// ---- small string helpers -------------------------------------------------
std::string lower(std::string s) {
  for (auto& c : s) c = (char)tolower((unsigned char)c);
  return s;
}
bool ieq(const std::string& a, const std::string& b) {
  return lower(a) == lower(b);
}

std::string u16_to_utf8(const u16* s, size_t n) {
  std::string out;
  for (size_t i = 0; i < n; ++i) {
    u32 c = s[i];
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && s[i + 1] >= 0xDC00 &&
        s[i + 1] < 0xE000) {
      c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
      ++i;
    }
    if (c < 0x80) out += (char)c;
    else if (c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { out += (char)(0xE0 | (c >> 12)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
    else { out += (char)(0xF0 | (c >> 18)); out += (char)(0x80 | ((c >> 12) & 0x3F)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
  }
  return out;
}

std::u16string utf8_to_u16(const std::string& s) {
  std::u16string out;
  for (size_t i = 0; i < s.size();) {
    unsigned char c = (unsigned char)s[i];
    u32 cp = c;
    size_t len = 1;
    if (c >= 0xF0 && i + 3 < s.size()) { cp = ((c & 7) << 18) | ((s[i+1] & 0x3F) << 12) | ((s[i+2] & 0x3F) << 6) | (s[i+3] & 0x3F); len = 4; }
    else if (c >= 0xE0 && i + 2 < s.size()) { cp = ((c & 0xF) << 12) | ((s[i+1] & 0x3F) << 6) | (s[i+2] & 0x3F); len = 3; }
    else if (c >= 0xC0 && i + 1 < s.size()) { cp = ((c & 0x1F) << 6) | (s[i+1] & 0x3F); len = 2; }
    i += len;
    if (cp >= 0x10000) { cp -= 0x10000; out += (char16_t)(0xD800 + (cp >> 10)); out += (char16_t)(0xDC00 + (cp & 0x3FF)); }
    else out += (char16_t)cp;
  }
  return out;
}

std::u16string from_guest(const u16* s) {
  std::u16string r;
  if (!s) return r;
  while (*s) r += (char16_t)*s++;
  return r;
}

std::u16string from_ansi(const char* s) {
  return s ? utf8_to_u16(std::string(s)) : std::u16string();
}

bool is_int_resource(const void* p) { return (uintptr_t)p <= 0xFFFFu; }

// Decode a manifest blob (UTF-8 / UTF-8 BOM / UTF-16LE/BE with or without BOM).
bool decode_xml(const u8* b, size_t n, std::string* out) {
  if (!b || n == 0) return false;
  out->clear();
  auto utf16 = [&](size_t off, bool be) {
    std::u16string tmp;
    for (size_t i = off; i + 1 < n; i += 2) {
      u16 c = be ? (u16)((b[i] << 8) | b[i + 1]) : (u16)(b[i] | (b[i + 1] << 8));
      if (c == 0) break;
      tmp += (char16_t)c;
    }
    *out = u16_to_utf8((const u16*)tmp.data(), tmp.size());
    return !out->empty();
  };
  if (n >= 2 && b[0] == 0xFF && b[1] == 0xFE) return utf16(2, false);
  if (n >= 2 && b[0] == 0xFE && b[1] == 0xFF) return utf16(2, true);
  if (n >= 4 && b[0] == '<' && b[1] == 0 && b[2] != 0 && b[3] == 0) return utf16(0, false);
  size_t off = (n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) ? 3 : 0;
  size_t end = n;
  while (end > off && b[end - 1] == 0) --end;  // trailing NUL padding in .rsrc
  out->assign((const char*)b + off, end - off);
  return !out->empty();
}

// ---- tiny tag scanner -----------------------------------------------------
struct Tag {
  std::string name;  // lowercase local name
  std::map<std::string, std::string> attrs;  // lowercase attr -> original value
  std::string text;
  bool in_dependent = false;
  bool in_windows_settings = false;
};

std::string local_name(const std::string& q) {
  size_t c = q.rfind(':');
  return lower(c == std::string::npos ? q : q.substr(c + 1));
}

std::vector<Tag> scan_tags(const std::string& xml) {
  std::vector<Tag> tags;
  std::vector<std::string> stack;
  size_t i = 0;
  while (i < xml.size()) {
    size_t lt = xml.find('<', i);
    if (lt == std::string::npos) break;
    if (xml.compare(lt, 4, "<!--") == 0) {
      size_t e = xml.find("-->", lt + 4);
      i = e == std::string::npos ? xml.size() : e + 3;
      continue;
    }
    if (lt + 1 >= xml.size()) break;
    char nx = xml[lt + 1];
    size_t gt = xml.find('>', lt + 1);
    if (gt == std::string::npos) break;
    if (nx == '?' || nx == '!') { i = gt + 1; continue; }
    if (nx == '/') {
      if (!stack.empty()) stack.pop_back();
      i = gt + 1;
      continue;
    }
    bool self_close = gt > lt && xml[gt - 1] == '/';
    size_t p = lt + 1;
    size_t ne = p;
    while (ne < gt && !isspace((unsigned char)xml[ne]) && xml[ne] != '/') ++ne;
    Tag t;
    t.name = local_name(xml.substr(p, ne - p));
    // attributes
    size_t a = ne;
    while (a < gt) {
      while (a < gt && (isspace((unsigned char)xml[a]) || xml[a] == '/')) ++a;
      size_t ks = a;
      while (a < gt && xml[a] != '=' && !isspace((unsigned char)xml[a])) ++a;
      std::string key = xml.substr(ks, a - ks);
      while (a < gt && isspace((unsigned char)xml[a])) ++a;
      if (a >= gt || xml[a] != '=') { if (key.empty()) break; continue; }
      ++a;
      while (a < gt && isspace((unsigned char)xml[a])) ++a;
      if (a >= gt || (xml[a] != '"' && xml[a] != '\'')) break;
      char q = xml[a++];
      size_t ve = xml.find(q, a);
      if (ve == std::string::npos || ve > gt) break;
      t.attrs[local_name(key)] = xml.substr(a, ve - a);
      a = ve + 1;
    }
    for (auto& s : stack) {
      if (s == "dependentassembly") t.in_dependent = true;
      if (s == "windowssettings") t.in_windows_settings = true;
    }
    if (!self_close) {
      size_t nlt = xml.find('<', gt + 1);
      std::string txt = xml.substr(gt + 1, (nlt == std::string::npos ? xml.size() : nlt) - gt - 1);
      size_t f = 0, l = txt.size();
      while (f < l && isspace((unsigned char)txt[f])) ++f;
      while (l > f && isspace((unsigned char)txt[l - 1])) --l;
      t.text = txt.substr(f, l - f);
      stack.push_back(t.name);
    }
    tags.push_back(std::move(t));
    i = gt + 1;
  }
  return tags;
}

std::string attr(const Tag& t, const char* k) {
  auto it = t.attrs.find(k);
  return it == t.attrs.end() ? std::string() : it->second;
}

Ident ident_of(const Tag& t) {
  Ident id;
  id.name = attr(t, "name");
  id.version = attr(t, "version");
  id.arch = attr(t, "processorarchitecture");
  id.token = attr(t, "publickeytoken");
  id.lang = attr(t, "language");
  id.type = attr(t, "type");
  return id;
}

bool parse_manifest(const u8* b, size_t n, Manifest* m) {
  if (!decode_xml(b, n, &m->xml)) return false;
  if (lower(m->xml).find("<assembly") == std::string::npos) return false;
  bool have_self = false;
  for (const Tag& t : scan_tags(m->xml)) {
    if (t.name == "assemblyidentity") {
      Ident id = ident_of(t);
      if (t.in_dependent) {
        if (!id.name.empty()) {
          m->deps.push_back(id);
          if (ieq(id.name, "Microsoft.Windows.Common-Controls") &&
              id.version.rfind("6.", 0) == 0)
            m->comctl6 = true;
        }
      } else if (!have_self) {
        m->self = id;
        have_self = true;
      }
    } else if (t.name == "file") {
      std::string f = attr(t, "name");
      if (!f.empty()) m->files.push_back(f);
    } else if (t.name == "requestedexecutionlevel") {
      std::string lv = lower(attr(t, "level"));
      m->run_level = lv == "asinvoker" ? 1 : lv == "highestavailable" ? 2
                   : lv == "requireadministrator" ? 3 : 0;
      m->ui_access = lower(attr(t, "uiaccess")) == "true";
    } else if (t.name == "supportedos") {
      std::string id = lower(attr(t, "id"));
      if (!id.empty()) m->supported_os.push_back(id);
    } else if (t.in_windows_settings && !t.text.empty()) {
      m->settings[t.name] = t.text;
    }
  }
  return true;
}

// ---- IO -------------------------------------------------------------------
bool read_guest_file(const std::u16string& guest, std::string* bytes) {
  std::u16string z = guest;
  char host[1024];
  if (trt_vfs_resolve_utf16((const u16*)z.c_str(), host, sizeof(host)) != 0)
    return false;
  FILE* f = fopen(host, "rb");
  if (!f) return false;
  std::string data;
  char buf[4096];
  size_t got;
  while ((got = fread(buf, 1, sizeof(buf), f)) > 0 && data.size() < (4u << 20))
    data.append(buf, got);
  fclose(f);
  *bytes = std::move(data);
  return !bytes->empty();
}

bool load_manifest_resource(void* hmod, const void* name, u16 lang, Manifest* m) {
  void* h = trt_FindResourceExW(hmod, (const u16*)(uintptr_t)24, (const u16*)name, lang);
  if (!h && lang) h = trt_FindResourceExW(hmod, (const u16*)(uintptr_t)24, (const u16*)name, 0);
  if (!h) return false;
  u32 size = trt_SizeofResource(hmod, h);
  void* g = trt_LoadResource(hmod, h);
  const u8* data = g ? (const u8*)trt_LockResource(g) : nullptr;
  if (!data || !size) return false;
  return parse_manifest(data, size, m);
}

std::u16string dir_of(const std::u16string& p) {
  size_t s = p.find_last_of(u"\\/");
  return s == std::u16string::npos ? std::u16string() : p.substr(0, s);
}

std::u16string basename_of(const std::u16string& p) {
  size_t s = p.find_last_of(u"\\/");
  return s == std::u16string::npos ? p : p.substr(s + 1);
}

// ---- registry of contexts ---------------------------------------------------
std::shared_ptr<Ctx> lookup(u64 token) {
  if (!token || token == kInvalidHandle) return nullptr;
  std::lock_guard<std::mutex> g(g_mu);
  auto it = g_ctxs.find(token);
  return it == g_ctxs.end() ? nullptr : it->second;
}

u64 store(std::shared_ptr<Ctx> c) {
  std::lock_guard<std::mutex> g(g_mu);
  u64 t = g_next_token.fetch_add(1);
  while (t == 0 || t == kInvalidHandle || g_ctxs.count(t)) t = g_next_token.fetch_add(1);
  g_ctxs.emplace(t, std::move(c));
  return t;
}

u64 current_token() {
  return t_active.empty() ? g_process_default.load() : t_active.back().ctx;
}

// ---- CreateActCtx -----------------------------------------------------------
// ACTCTXW on x64: 56 bytes. Natural alignment reproduces the Win64 layout.
struct ActCtxW {
  u32 cbSize;
  u32 dwFlags;
  const u16* lpSource;
  u16 wProcessorArchitecture;
  u16 wLangId;
  const u16* lpAssemblyDirectory;
  const u16* lpResourceName;
  const u16* lpApplicationName;
  void* hModule;
};
static_assert(sizeof(ActCtxW) == 56, "ACTCTXW must match the Win64 ABI");

void* create_from(const ActCtxW* c) {
  if (!c || c->cbSize < sizeof(ActCtxW)) {
    trt_SetLastError(kErrInvalidParam);
    return (void*)(uintptr_t)kInvalidHandle;
  }
  const u32 f = c->dwFlags;
  auto ctx = std::make_shared<Ctx>();
  std::u16string source = from_guest(c->lpSource);
  ctx->m.source = source;
  if ((f & kFlagAsmDir) && c->lpAssemblyDirectory)
    ctx->m.app_dir = from_guest(c->lpAssemblyDirectory);
  else
    ctx->m.app_dir = dir_of(source);

  const void* res_name = (f & kFlagResName) ? (const void*)c->lpResourceName
                                            : (const void*)(uintptr_t)1;
  const u16 lang = (f & kFlagLang) ? c->wLangId : 0;
  if (f & kFlagResName) {
    if (is_int_resource(res_name))
      ctx->resource_name = u"#" + utf8_to_u16(std::to_string((unsigned long)(uintptr_t)res_name));
    else
      ctx->resource_name = from_guest(c->lpResourceName);
  }

  bool ok = false;
  void* hmod = (f & kFlagModule) ? c->hModule : nullptr;
  if (!hmod && !source.empty() && (f & kFlagResName)) {
    // Source names an already-loaded image (the console has no WinSxS to
    // open it as a datafile): find it by basename.
    std::string base = u16_to_utf8((const u16*)basename_of(source).data(),
                                   basename_of(source).size());
    if (trt_image* img = trt_images_find(base.c_str())) hmod = trt_images_handle(img);
  }
  if (hmod) {
    ok = load_manifest_resource(hmod, res_name, lang, &ctx->m);
  } else if (!source.empty()) {
    std::string bytes;
    if (!read_guest_file(source, &bytes)) {
      trt_SetLastError(kErrFileNotFound);
      return (void*)(uintptr_t)kInvalidHandle;
    }
    ok = parse_manifest((const u8*)bytes.data(), bytes.size(), &ctx->m);
  }
  if (!ok) {
    trt_SetLastError(kErrSxsCantGen);
    return (void*)(uintptr_t)kInvalidHandle;
  }
  u64 tok = store(ctx);
  if (f & kFlagSetProcessDefault) g_process_default.store(tok);
  trt_SetLastError(0);
  return (void*)(uintptr_t)tok;
}

void* TRT_STDCALL k_CreateActCtxW(const ActCtxW* c) { return create_from(c); }

struct ActCtxA {
  u32 cbSize;
  u32 dwFlags;
  const char* lpSource;
  u16 wProcessorArchitecture;
  u16 wLangId;
  const char* lpAssemblyDirectory;
  const char* lpResourceName;
  const char* lpApplicationName;
  void* hModule;
};

void* TRT_STDCALL k_CreateActCtxA(const ActCtxA* a) {
  if (!a || a->cbSize < sizeof(ActCtxA)) {
    trt_SetLastError(kErrInvalidParam);
    return (void*)(uintptr_t)kInvalidHandle;
  }
  std::u16string src = from_ansi(a->lpSource), dir = from_ansi(a->lpAssemblyDirectory),
                 app = from_ansi(a->lpApplicationName), rn;
  ActCtxW w{};
  w.cbSize = sizeof(w);
  w.dwFlags = a->dwFlags;
  w.lpSource = a->lpSource ? (const u16*)src.c_str() : nullptr;
  w.wProcessorArchitecture = a->wProcessorArchitecture;
  w.wLangId = a->wLangId;
  w.lpAssemblyDirectory = a->lpAssemblyDirectory ? (const u16*)dir.c_str() : nullptr;
  w.lpApplicationName = a->lpApplicationName ? (const u16*)app.c_str() : nullptr;
  w.hModule = a->hModule;
  if (a->dwFlags & kFlagResName) {
    if (is_int_resource(a->lpResourceName)) w.lpResourceName = (const u16*)a->lpResourceName;
    else { rn = from_ansi(a->lpResourceName); w.lpResourceName = (const u16*)rn.c_str(); }
  }
  return create_from(&w);
}

void TRT_STDCALL k_AddRefActCtx(void* h) {
  if (auto c = lookup((u64)(uintptr_t)h)) c->refs.fetch_add(1);
}

void TRT_STDCALL k_ReleaseActCtx(void* h) {
  const u64 t = (u64)(uintptr_t)h;
  auto c = lookup(t);
  if (!c) return;
  if (c->refs.fetch_sub(1) == 1 && g_process_default.load() != t) {
    std::lock_guard<std::mutex> g(g_mu);
    g_ctxs.erase(t);
  }
}

int TRT_STDCALL k_ActivateActCtx(void* h, u64* cookie) {
  const u64 t = (u64)(uintptr_t)h;
  if (t != 0 && t != kInvalidHandle && !lookup(t)) {
    trt_SetLastError(kErrInvalidHandle);
    return 0;
  }
  u64 ck = g_next_cookie.fetch_add(1);
  t_active.push_back({t == kInvalidHandle ? 0 : t, ck});
  if (cookie) *cookie = ck;
  trt_SetLastError(0);
  return 1;
}

int TRT_STDCALL k_DeactivateActCtx(u32 flags, u64 cookie) {
  size_t idx = t_active.size();
  for (size_t i = t_active.size(); i-- > 0;)
    if (t_active[i].cookie == cookie) { idx = i; break; }
  if (idx == t_active.size()) {
    trt_SetLastError(kErrSxsInvalidDeact);
    return 0;
  }
  if (idx + 1 != t_active.size() && !(flags & kDeactForceEarly)) {
    trt_SetLastError(kErrSxsEarlyDeact);
    return 0;
  }
  t_active.resize(idx);
  trt_SetLastError(0);
  return 1;
}

int TRT_STDCALL k_GetCurrentActCtx(void** out) {
  if (!out) { trt_SetLastError(kErrInvalidParam); return 0; }
  const u64 t = current_token();
  *out = (void*)(uintptr_t)t;
  if (t) k_AddRefActCtx((void*)(uintptr_t)t);  // caller must Release (Win32 contract)
  trt_SetLastError(0);
  return 1;
}

int TRT_STDCALL k_ZombifyActCtx(void* h) {
  if (!lookup((u64)(uintptr_t)h)) { trt_SetLastError(kErrInvalidHandle); return 0; }
  return 1;  // contexts hold no external resources; nothing to invalidate
}

// ---- Query ------------------------------------------------------------------
bool parse_guid(const std::string& s, u8 out[16]) {
  unsigned d1, d2, d3, b[8];
  if (sscanf(s.c_str(), "{%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x}", &d1, &d2, &d3,
             &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7]) != 11)
    return false;
  memcpy(out, &d1, 4);
  u16 w2 = (u16)d2, w3 = (u16)d3;
  memcpy(out + 4, &w2, 2);
  memcpy(out + 6, &w3, 2);
  for (int i = 0; i < 8; ++i) out[8 + i] = (u8)b[i];
  return true;
}

int finish_fixed(const void* src, size_t n, void* buf, u64 size, u64* required) {
  if (required) *required = n;
  if (!buf || size < n) { trt_SetLastError(kErrInsufficient); return 0; }
  memcpy(buf, src, n);
  trt_SetLastError(0);
  return 1;
}

int TRT_STDCALL k_QueryActCtxW(u32 flags, void* h, void*, u32 cls, void* buf,
                               u64 size, u64* required) {
  if (flags & ~(kQueryUseActive | kQueryNoAddRef)) {
    trt_SetLastError(kErrNotSupported);
    return 0;
  }
  const u64 sel = (flags & kQueryUseActive) ? current_token() : (u64)(uintptr_t)h;
  auto c = lookup(sel);
  if (!c) { trt_SetLastError(kErrInvalidHandle); return 0; }
  switch (cls) {
    case 1: {  // ActivationContextBasicInformation {HANDLE; DWORD}
      struct { u64 h; u32 f; u32 pad; } info = {sel, 0, 0};
      int ok = finish_fixed(&info, sizeof(info), buf, size, required);
      if (ok && !(flags & kQueryNoAddRef)) c->refs.fetch_add(1);
      return ok;
    }
    case 2: {  // ActivationContextDetailedInformation
      const size_t chars = c->m.source.size();
      const size_t hdr = 64, total = hdr + (chars + 1) * 2;
      if (required) *required = total;
      if (!buf || size < total) { trt_SetLastError(kErrInsufficient); return 0; }
      u8* p = (u8*)buf;
      memset(p, 0, hdr);
      u32 v[9] = {0, 1, (u32)(1 + c->m.deps.size()), 1, (u32)chars, 0, 0, 0, 0};
      memcpy(p, v, sizeof(v));
      u16* text = (u16*)(p + hdr);
      for (size_t i = 0; i < chars; ++i) text[i] = (u16)c->m.source[i];
      text[chars] = 0;
      u64 ptr = (u64)(uintptr_t)text;
      memcpy(p + 40, &ptr, 8);
      trt_SetLastError(0);
      return 1;
    }
    case 5: {  // RunlevelInformationInActivationContext {ulFlags, RunLevel, UiAccess}
      u32 info[3] = {0, (u32)c->m.run_level, c->m.ui_access ? 1u : 0u};
      return finish_fixed(info, sizeof(info), buf, size, required);
    }
    case 6: {  // CompatibilityInformationInActivationContext
      const size_t count = c->m.supported_os.size();
      const size_t total = 8 + count * 32;
      if (required) *required = total;
      if (!buf || size < total) { trt_SetLastError(kErrInsufficient); return 0; }
      u8* p = (u8*)buf;
      memset(p, 0, total);
      u32 n = (u32)count;
      memcpy(p, &n, 4);
      for (size_t i = 0; i < count; ++i) {
        u8* e = p + 8 + i * 32;
        parse_guid(c->m.supported_os[i], e);
        u32 type = 1;  // ACTCTX_COMPATIBILITY_ELEMENT_TYPE_OS
        memcpy(e + 16, &type, 4);
      }
      trt_SetLastError(0);
      return 1;
    }
    default:
      trt_SetLastError(kErrNotSupported);
      return 0;
  }
}

int TRT_STDCALL k_QueryActCtxSettingsW(u32 flags, void* h, const u16*,
                                       const u16* setting, u16* buf, u64 count,
                                       u64* written) {
  if (flags != 0 || !setting) { trt_SetLastError(kErrInvalidParam); return 0; }
  auto c = lookup(h ? (u64)(uintptr_t)h : current_token());
  if (!c) { trt_SetLastError(kErrInvalidHandle); return 0; }
  std::string key;
  for (const u16* s = setting; *s; ++s) key += (char)tolower((int)*s & 0x7F);
  auto it = c->m.settings.find(key);
  if (it == c->m.settings.end() || it->second.empty()) {
    trt_SetLastError(kErrSxsKeyNotFound);
    return 0;
  }
  std::u16string v = utf8_to_u16(it->second);
  const u64 req = v.size() + 1;
  if (written) *written = req;
  if (!buf || count < req) { trt_SetLastError(kErrInsufficient); return 0; }
  for (size_t i = 0; i < v.size(); ++i) buf[i] = (u16)v[i];
  buf[v.size()] = 0;
  trt_SetLastError(0);
  return 1;
}

// A runtime with no WinSxS store has no assembly sections to enumerate.
int TRT_STDCALL k_FindActCtxSectionStringW(u32, const void*, u32, const u16*, void*) {
  trt_SetLastError(kErrSxsKeyNotFound);
  return 0;
}
int TRT_STDCALL k_FindActCtxSectionStringA(u32, const void*, u32, const char*, void*) {
  trt_SetLastError(kErrSxsKeyNotFound);
  return 0;
}
int TRT_STDCALL k_FindActCtxSectionGuid(u32, const void*, u32, const void*, void*) {
  trt_SetLastError(kErrSxsKeyNotFound);
  return 0;
}

}  // namespace

extern "C" {

int trt_actctx_current_info(trt_actctx_info* out) {
  if (!out) return 0;
  memset(out, 0, sizeof(*out));
  auto c = lookup(current_token());
  if (!c) return 0;
  out->valid = 1;
  out->common_controls_v6 = c->m.comctl6;
  out->long_path_aware = ieq(c->m.settings.count("longpathaware") ? c->m.settings["longpathaware"] : "", "true");
  out->ui_access = c->m.ui_access;
  out->run_level = c->m.run_level;
  out->dependency_count = (int)c->m.deps.size();
  std::string dpi = c->m.settings.count("dpiawareness") ? c->m.settings["dpiawareness"]
                  : c->m.settings.count("dpiaware") ? c->m.settings["dpiaware"] : "";
  std::string d = lower(dpi);
  out->dpi_aware = d.find("permonitor") != std::string::npos ? 2
                 : (d.find("true") != std::string::npos || d.find("system") != std::string::npos) ? 1 : 0;
  return 1;
}

void* trt_actctx_install_process_default(void* hmod) {
  auto ctx = std::make_shared<Ctx>();
  if (!load_manifest_resource(hmod, (const void*)(uintptr_t)1, 0, &ctx->m)) return nullptr;
  u64 tok = store(ctx);
  g_process_default.store(tok);
  return (void*)(uintptr_t)tok;
}

void trt_actctx_reset(void) {
  std::lock_guard<std::mutex> g(g_mu);
  g_ctxs.clear();
  g_process_default.store(0);
  t_active.clear();
}

int trt_bridge_register_actctx(void) {
  static const trt_named_fn k_fns[] = {
      {"CreateActCtxW", (void*)&k_CreateActCtxW},
      {"CreateActCtxA", (void*)&k_CreateActCtxA},
      {"AddRefActCtx", (void*)&k_AddRefActCtx},
      {"ReleaseActCtx", (void*)&k_ReleaseActCtx},
      {"ActivateActCtx", (void*)&k_ActivateActCtx},
      {"DeactivateActCtx", (void*)&k_DeactivateActCtx},
      {"GetCurrentActCtx", (void*)&k_GetCurrentActCtx},
      {"ZombifyActCtx", (void*)&k_ZombifyActCtx},
      {"QueryActCtxW", (void*)&k_QueryActCtxW},
      {"QueryActCtxSettingsW", (void*)&k_QueryActCtxSettingsW},
      {"FindActCtxSectionStringW", (void*)&k_FindActCtxSectionStringW},
      {"FindActCtxSectionStringA", (void*)&k_FindActCtxSectionStringA},
      {"FindActCtxSectionGuid", (void*)&k_FindActCtxSectionGuid},
  };
  const int n = (int)(sizeof(k_fns) / sizeof(k_fns[0]));
  trt_module_register("kernel32.dll", TRT_ROUTE_BUILTIN, k_fns, n);
  trt_module_register("kernelbase.dll", TRT_ROUTE_BUILTIN, k_fns, n);
  return 0;
}

}  // extern "C"
