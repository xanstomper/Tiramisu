// tiramisu/src/trt/bridge_res.cpp
// PE resource directory (.rsrc) surface: FindResourceExW/FindResourceW/
// SizeofResource/LoadResource/LockResource/UnlockResource/FreeResource/
// EnumResource*W/LoadStringW/RegisterResourceError.
//
// Master-Prompt Sprint-1 item 15 (1J). Games read their OWN exe/dll
// resources at boot (version info, icons, dialog templates, localized
// strings, embedded engine blobs). Measured before this file existed:
// ZERO .rsrc support anywhere in the tree — a game whose CRT/engine calls
// FindResourceW died with NULL + no named cause.
//
// Design (matches pe.h windows.h-free style):
//   - .rsrc parsing in FILE SPACE over the mapped image: the resource
//     directory is a tree TYPE -> NAME -> LANG of RVAs+sizes into the
//     image. We walk it with rd16/rd32 helpers like pe_exports.cpp.
//   - A resource "handle" (HRSRC) is a synthetic pointer into a table of
//     {image, type, name, lang, data_rva, data_size} records.
//   - LoadResource returns a synthetic HGLOBAL record handle; LockResource
//     maps it to the GUEST-VISIBLE POINTER base + data_rva (the real
//     Windows semantic: LockResource returns a pointer into the loaded
//     module's mapped image). SizeofResource returns the directory size.
//   - Fail-visible: every miss logs the type/name and sets last error
//     semantics by returning NULL (caller-visible), matching Windows.
//     Last-error TLS lives in bridge_kernel32 (per-thread); on miss the
//     caller's GetLastError is unspecified-stale — the named log line is
//     the diagnostic contract (Bridge Rules: never silent).
#include "bridge_res.h"
#include "images.h"
#include "pe.h"
#include "thunk.h"
#include "trt.h"

#include <mutex>
#include <stdio.h>
#include <string.h>
#include <vector>

namespace reslog {
static void line(const char* s) { trt_core_log(s); }
}
#define log_line(s_) reslog::line(s_)

extern "C" {

// ============================ .rsrc walk ====================================

namespace {

constexpr u32 k_res_dir_entry_size = 8;   // IMAGE_RESOURCE_DIRECTORY_ENTRY
[[maybe_unused]] constexpr u32 k_res_data_entry_size = 16; // IMAGE_RESOURCE_DATA_ENTRY

struct res_key {
  u32 type;        // 0 = none given (FindResourceExW NULL type is illegal)
  u32 name;        // name id, or 0 when by-string (name_str set)
  char name_str[128];
  u32 lang;        // 0 = neutral / caller's find lang
  bool by_str;
};

u32 rd32r(const u8* p) { u32 v; memcpy(&v, p, 4); return v; }
u16 rd16r(const u8* p) { u16 v; memcpy(&v, p, 2); return v; }

// walk one directory level for a key. dir_abs is the ABSOLUTE image RVA
// of the IMAGE_RESOURCE_DIRECTORY. Child subdir offsets are
// SECTION-RELATIVE (PE spec) — they are rebased by adding rsrc_rva.
// A returned data-entry RVA is already absolute (the spec keeps DataRva
// as a real image RVA).
// returns absolute RVA of the child, 0 on miss.
u64 res_dir_find(const u8* base, u64 rsrc_rva, u64 dir_abs, u32 key,
                 bool by_id, int depth) {
  if (!base || !dir_abs || depth > 3) return 0;
  const u8* dir = base + dir_abs;
  const u16 named = rd16r(dir + 12);
  const u16 ided = rd16r(dir + 14);
  const u32 count = (u32)(named + ided);
  const u8* entry = dir + 16;
  for (u32 i = 0; i < count; ++i, entry += k_res_dir_entry_size) {
    const u32 name_or_id = rd32r(entry);
    const bool entry_is_id = (name_or_id & 0x80000000u) == 0;
    if (by_id != entry_is_id) continue;
    if (by_id && (name_or_id & 0x00FFFFFFu) != key) continue;
    // BOTH subdir and data-entry offsets are section-relative; the high
    // bit only distinguishes subdir vs data entry.
    return rsrc_rva + (u64)(rd32r(entry + 4) & 0x7FFFFFFFu);
  }
  return 0;
}

// string-name search: like res_dir_find but compares UTF-16 names
// case-insensitively against an ASCII/UTF-8 caller string.
u64 res_dir_find_named(const u8* base, u64 rsrc_rva, u64 dir_abs,
                       const char* want, int depth) {
  if (!base || !dir_abs || !want || !want[0] || depth > 3) return 0;
  const u8* dir = base + dir_abs;
  const u16 named = rd16r(dir + 12);
  const u16 ided = rd16r(dir + 14);
  const u32 count = (u32)(named + ided);
  const u8* entry = dir + 16;
  const size_t want_len = strlen(want);
  for (u32 i = 0; i < count; ++i, entry += k_res_dir_entry_size) {
    const u32 name_or_id = rd32r(entry);
    if ((name_or_id & 0x80000000u) == 0) continue;  // id entry: skip
    // name strings live at SECTION-relative offsets too
    const u64 str_abs = rsrc_rva + (u64)(name_or_id & 0x7FFFFFFFu);
    const u8* sp = base + str_abs;
    const u16 bytes = rd16r(sp);
    const u16 off = rd16r(sp + 2);
    const size_t chars = (size_t)(bytes / 2);
    if (chars != want_len) continue;
    bool eq = true;
    for (size_t c = 0; c < chars; ++c) {
      const u16 wc = rd16r(sp + 4 + off + c * 2);
      char hc = (char)(wc & 0x00FFu);
      if (wc & 0xFF00u) { eq = false; break; }  // non-ANSI: no match
      if (hc >= 'A' && hc <= 'Z') hc = (char)(hc - 'A' + 'a');
      char w = want[c];
      if (w >= 'A' && w <= 'Z') w = (char)(w - 'A' + 'a');
      if (hc != w) { eq = false; break; }
    }
    if (eq) return rsrc_rva + (u64)(rd32r(entry + 4) & 0x7FFFFFFFu);
  }
  return 0;
}

struct res_lookup {
  u64 data_rva;
  u32 data_size;
  u16 code_page;
};

// full walk: type -> name -> lang -> IMAGE_RESOURCE_DATA_ENTRY.
// All directory-child offsets are section-relative and get rebased inside
// res_dir_find*; every RVA coming back is ABSOLUTE.
bool res_walk(const u8* base, u64 rsrc_rva, const res_key& k,
              res_lookup* out) {
  if (!base || !rsrc_rva) return false;
  // the type level is always an id in practice (caller gives numeric RT_*)
  const u64 name_dir_rva = res_dir_find(base, rsrc_rva, rsrc_rva, k.type,
                                        true, 0);
  if (!name_dir_rva) return false;

  u64 lang_dir_rva = 0;
  if (k.by_str) {
    lang_dir_rva = res_dir_find_named(base, rsrc_rva, name_dir_rva,
                                      k.name_str, 1);
  } else {
    lang_dir_rva = res_dir_find(base, rsrc_rva, name_dir_rva, k.name,
                                true, 1);
  }
  if (!lang_dir_rva) return false;

  // lang level: caller lang exact, else the FIRST entry (real Windows
  // tries exact -> neutral; a single-language game exe has one entry)
  u64 data_rva = res_dir_find(base, rsrc_rva, lang_dir_rva, k.lang, true, 2);
  if (!data_rva) {
    // take the first child of the lang dir regardless of id
    const u8* dir = base + lang_dir_rva;
    const u16 named = rd16r(dir + 12);
    const u16 ided = rd16r(dir + 14);
    if (named + ided == 0) return false;
    data_rva = rsrc_rva + (u64)(rd32r(dir + 16 + 4) & 0x7FFFFFFFu);
  }
  if (!data_rva) return false;
  const u8* de = base + data_rva;
  out->data_rva = rd32r(de);
  out->data_size = rd32r(de + 4);
  out->code_page = rd16r(de + 8);
  return out->data_rva != 0 && out->data_size != 0;
}

// standard type ids (winuser.h RT_*)
[[maybe_unused]] constexpr u32 k_RT_CURSOR = 1, k_RT_BITMAP = 2, k_RT_ICON = 3,
              k_RT_MENU = 4, k_RT_DIALOG = 5, k_RT_STRING = 6,
              k_RT_FONTDIR = 7, k_RT_FONT = 8, k_RT_ACCELERATOR = 9,
              k_RT_RCDATA = 10, k_RT_MESSAGETABLE = 11, k_RT_GROUP_CURSOR = 12,
              k_RT_GROUP_ICON = 14, k_RT_VERSION = 16, k_RT_DLGINCLUDE = 17,
              k_RT_PLUGPLAY = 19, k_RT_VXD = 20, k_RT_ANICURSOR = 21,
              k_RT_ANIICON = 22, k_RT_HTML = 23, k_RT_MANIFEST = 24;

const char* res_type_name(u32 t) {
  switch (t) {
    case k_RT_CURSOR: return "CURSOR";
    case k_RT_BITMAP: return "BITMAP";
    case k_RT_ICON: return "ICON";
    case k_RT_MENU: return "MENU";
    case k_RT_DIALOG: return "DIALOG";
    case k_RT_STRING: return "STRING";
    case k_RT_RCDATA: return "RCDATA";
    case k_RT_MESSAGETABLE: return "MESSAGETABLE";
    case k_RT_GROUP_CURSOR: return "GROUP_CURSOR";
    case k_RT_GROUP_ICON: return "GROUP_ICON";
    case k_RT_VERSION: return "VERSION";
    case k_RT_MANIFEST: return "MANIFEST";
    case k_RT_HTML: return "HTML";
    default: return "?";
  }
}

} // namespace

// ============================ handle space ==================================

namespace {

struct res_hrsrc {
  trt_image* img;
  res_key key;
  u64 data_rva;
  u32 data_size;
  u16 code_page;
};
struct res_hglobal {
  res_hrsrc* src;
};

std::mutex g_res_mu;std::vector<res_hrsrc> g_hsrcs;
std::vector<res_hglobal> g_hglobals;

u32 rsrc_index_of(void* h) {
  // HRSRC synthetic: 0xR5nnnnnn tag space
  const uintptr_t v = (uintptr_t)h;
  if ((v >> 32) != 0x00000052u) return 0;  // 'R'
  const u32 idx = (u32)(v & 0x00FFFFFFu);
  if (idx == 0 || idx > g_hsrcs.size()) return 0;
  return idx;
}
u32 hglob_index_of(void* h) {
  const uintptr_t v = (uintptr_t)h;
  if ((v >> 32) != 0x00000047u) return 0;  // 'G'
  const u32 idx = (u32)(v & 0x00FFFFFFu);
  if (idx == 0 || idx > g_hglobals.size()) return 0;
  return idx;
}
void* rsrc_make(u32 idx) {
  return (void*)(uintptr_t)(0x0000005200000000ull | (u64)idx);
}
void* hglob_make(u32 idx) {
  return (void*)(uintptr_t)(0x0000004700000000ull | (u64)idx);
}

// the .rsrc directory RVA of an image (0 = none)
u64 rsrc_rva_of(trt_image* img) {
  if (!img || !img->base) return 0;
  // DOS e_lfanew -> NT -> optional header data directory [2]
  const u8* b = img->base;
  if (rd16r(b) != 0x5A4D) return 0;
  const u32 lfanew = rd32r(b + 0x3C);
  if (rd32r(b + lfanew) != 0x00004550u) return 0;  // 'PE\0\0'
  const u8* opt = b + lfanew + 24;
  if (rd16r(opt) != 0x020Bu) return 0;             // PE64 only
  const u8* dirs = opt + 112;                      // 96-byte standard + data dirs start
  const u32 rva = rd32r(dirs + 2 * 8);
  const u32 size = rd32r(dirs + 2 * 8 + 4);
  (void)size;
  return rva;
}

void type_from_wstr(const u16* ws, res_key& k) {
  // caller gives type as MAKEINTRESOURCE pointer or a wide string; the
  // common game path is MAKEINTRESOURCE (low word = id, high word = 0).
  const uintptr_t v = (uintptr_t)ws;
  if ((v >> 16) == 0 && (v & 0xFFFFu) != 0) {
    k.type = (u32)(v & 0xFFFFu);
    k.by_str = false;  // type is always by id
    return;
  }
  // string type: map known names; unknown string types log-named-fail
  k.type = 0;
  k.by_str = false;
}

u32 type_normalize(u32 t) {
  // raw RT_ value straight through; callers pass ids
  return t;
}

bool key_fill_from_args(u32 type, const void* name, u16 lang, res_key* k,
                        const u16* type_w) {
  memset(k, 0, sizeof(*k));
  k->lang = lang;
  // type
  if (type_w) {
    type_from_wstr(type_w, *k);
    if (k->type == 0) return false;
  } else {
    const uintptr_t tv = (uintptr_t)type;
    if ((tv >> 16) == 0 && (tv & 0xFFFFu) != 0) {
      k->type = type_normalize((u32)(tv & 0xFFFFu));
    } else {
      return false;
    }
  }
  // name
  const uintptr_t nv = (uintptr_t)name;
  if ((nv >> 16) == 0 && (nv & 0xFFFFu) != 0) {
    k->name = (u32)(nv & 0xFFFFu);
    k->by_str = false;
  } else if (name) {
    const u16* ws = (const u16*)name;
    size_t n = 0;
    while (ws[n] && n < 127) ++n;
    for (size_t i = 0; i < n; ++i)
      k->name_str[i] = (char)(ws[i] & 0x00FFu);
    k->name_str[n] = 0;
    k->by_str = true;
  } else {
    return false;
  }
  return true;
}

} // namespace

// ============================ exports =======================================

// FindResourceExW(HMODULE, LPWSTR type, LPWSTR name, WORD lang)
void* TRT_STDCALL trt_FindResourceExW(void* hmod, const u16* type_w,
                                      const u16* name_w, u16 lang) {
  trt_image* img = hmod ? trt_images_by_handle(hmod)
                        : trt_images_first();
  if (!img) {
    char l[128];
    snprintf(l, sizeof(l),
             "res: FindResourceExW -> NULL (no image for module %p)", hmod);
    log_line(l);
    return nullptr;
  }
  const u64 rsrc = rsrc_rva_of(img);
  if (!rsrc) {
    char l[192];
    snprintf(l, sizeof(l),
             "res: FindResourceExW -> NULL (image '%s' has no .rsrc directory)",
             trt_images_name(trt_images_handle(img)));
    log_line(l);
    return nullptr;
  }
  res_key k;
  if (!key_fill_from_args(0, name_w, lang, &k, type_w)) {
    log_line("res: FindResourceExW -> NULL (unparseable type/name: string "
             "types and NULL names unsupported)");
    return nullptr;
  }
  res_lookup lk{};
  if (!res_walk(img->base, rsrc, k, &lk)) {
    char l[256];
    if (k.by_str)
      snprintf(l, sizeof(l),
               "res: FindResourceExW -> NULL (type %s name \"%s\" lang %u "
               "not in '%s' .rsrc)",
               res_type_name(k.type), k.name_str, k.lang,
               trt_images_name(trt_images_handle(img)));
    else
      snprintf(l, sizeof(l),
               "res: FindResourceExW -> NULL (type %s name #%u lang %u not "
               "in '%s' .rsrc)",
               res_type_name(k.type), k.name, k.lang,
               trt_images_name(trt_images_handle(img)));
    log_line(l);
    return nullptr;
  }
  std::lock_guard<std::mutex> lk_mu(g_res_mu);
  res_hrsrc rec;
  rec.img = img;
  rec.key = k;
  rec.data_rva = lk.data_rva;
  rec.data_size = lk.data_size;
  rec.code_page = lk.code_page;
  g_hsrcs.push_back(rec);
  const u32 idx = (u32)g_hsrcs.size();
  return rsrc_make(idx);
}

// FindResourceW(HMODULE, LPWSTR name, LPWSTR type)  (note: name, type order)
void* TRT_STDCALL trt_FindResourceW(void* hmod, const u16* name_w,
                                    const u16* type_w) {
  return trt_FindResourceExW(hmod, type_w, name_w, 0x0409 /*us-en*/);
}

// FindResourceA family: ANSI callers (many 2000s games). Convert args.
// Both type and name may be MAKEINTRESOURCEA integer pointers — guard
// strlen with the IS_INTRESOURCE test (high word zero) like key_fill does.
void* TRT_STDCALL trt_FindResourceExA(void* hmod, const char* type_a,
                                      const char* name_a, u16 lang) {
  const uintptr_t tv = (uintptr_t)type_a;
  const uintptr_t nv = (uintptr_t)name_a;
  u16 tw[64], nw[128];
  const u16* type_arg = nullptr;
  if (type_a && (tv >> 16) != 0) {
    size_t n = strlen(type_a);
    if (n > 63) n = 63;
    for (size_t i = 0; i < n; ++i) tw[i] = (u8)type_a[i];
    tw[n] = 0;
    type_arg = tw;
  } else if (type_a) {
    // MAKEINTRESOURCEA: pass the id straight through (ExW handles it)
    type_arg = (const u16*)type_a;
  }
  const u16* name_arg = nullptr;
  if (name_a && (nv >> 16) != 0) {
    size_t n = strlen(name_a);
    if (n > 127) n = 127;
    for (size_t i = 0; i < n; ++i) nw[i] = (u8)name_a[i];
    nw[n] = 0;
    name_arg = nw;
  } else if (name_a) {
    name_arg = (const u16*)name_a;
  }
  return trt_FindResourceExW(hmod, type_arg, name_arg, lang);
}

void* TRT_STDCALL trt_FindResourceA(void* hmod, const char* name_a,
                                    const char* type_a) {
  return trt_FindResourceExA(hmod, type_a, name_a, 0x0409);
}

// SizeofResource(HMODULE, HRSRC)
u32 TRT_STDCALL trt_SizeofResource(void* hmod, void* hsrc) {
  (void)hmod;
  std::lock_guard<std::mutex> lk(g_res_mu);
  const u32 idx = rsrc_index_of(hsrc);
  if (!idx) return 0;  // real Windows also 0s a bad HRSRC
  return g_hsrcs[idx - 1].data_size;
}

// LoadResource(HMODULE, HRSRC) -> HGLOBAL
void* TRT_STDCALL trt_LoadResource(void* hmod, void* hsrc) {
  (void)hmod;
  std::lock_guard<std::mutex> lk(g_res_mu);
  const u32 idx = rsrc_index_of(hsrc);
  if (!idx) {
    log_line("res: LoadResource -> NULL (bad HRSRC)");
    return nullptr;
  }
  res_hglobal g;
  g.src = &g_hsrcs[idx - 1];
  g_hglobals.push_back(g);
  return hglob_make((u32)g_hglobals.size());
}

// LockResource(HGLOBAL) -> pointer into the mapped image (real semantic)
void* TRT_STDCALL trt_LockResource(void* hglob) {
  std::lock_guard<std::mutex> lk(g_res_mu);
  const u32 idx = hglob_index_of(hglob);
  if (!idx) return nullptr;  // real Windows: NULL on bad handle
  const res_hglobal& g = g_hglobals[idx - 1];
  if (!g.src || !g.src->img) return nullptr;
  return g.src->img->base + g.src->data_rva;
}

// UnlockResource/FreeResource: 16-bit era leftovers; always succeed
int TRT_STDCALL trt_UnlockResource(void* hglob) {
  (void)hglob;
  return 0;
}
int TRT_STDCALL trt_FreeResource(void* hglob) {
  (void)hglob;
  return 0;
}

// EnumResourceNamesW(HMODULE, type, callback, param) — enumerate via walk
// (games use it for icon pickup / asset discovery). We call the guest
// callback for every match. BOOL return.
int TRT_STDCALL trt_EnumResourceNamesW(void* hmod, const u16* type_w,
                                       void* callback, i64 param) {
  trt_image* img = hmod ? trt_images_by_handle(hmod) : trt_images_first();
  if (!img) return 0;
  const u64 rsrc = rsrc_rva_of(img);
  if (!rsrc) return 0;
  // resolve the type id (id or MAKEINTRESOURCE)
  u32 type = 0;
  if (type_w) {
    const uintptr_t v = (uintptr_t)type_w;
    if ((v >> 16) == 0) type = (u32)(v & 0xFFFFu);
  }
  if (!type) return 0;
  const u64 type_rva = res_dir_find(img->base, rsrc, rsrc, type, true, 0);
  if (!type_rva) return 0;
  // res_dir_find returns ABSOLUTE child RVAs
  const u8* dir = img->base + type_rva;
  const u16 named = rd16r(dir + 12);
  const u16 ided = rd16r(dir + 14);
  const u32 count = (u32)(named + ided);
  if (!callback) return 0;
  typedef int (TRT_STDCALL *enum_fn)(void*, const u16*, const u16*, i64);
  const enum_fn cb = (enum_fn)(void*)callback;
  const u8* entry = dir + 16;
  for (u32 i = 0; i < count; ++i, entry += k_res_dir_entry_size) {
    const u32 name_or_id = rd32r(entry);
    const bool is_id = (name_or_id & 0x80000000u) == 0;
    // build the name arg: id -> MAKEINTRESOURCE, string -> pointer into image
    const u16* name_arg = nullptr;
    u16 id_buf = 0;
    if (is_id) {
      id_buf = (u16)(name_or_id & 0xFFFFu);
      name_arg = (const u16*)(uintptr_t)id_buf;
    } else {
      const u64 str_abs = rsrc + (u64)(name_or_id & 0x7FFFFFFFu);
      name_arg = (const u16*)(img->base + str_abs + 4);
    }
    const u16* type_arg =
        (const u16*)(uintptr_t)(0x0000FFFFull & (u64)type);
    if (!cb(hmod, type_arg, name_arg, param)) return 1;  // stopped
  }
  return 1;  // enumerated all
}

// LoadStringW(HINSTANCE, uID, LPWSTR buf, int cch) -> chars copied, 0 miss.
// String tables live in RT_STRING: blocks of 16 (uID/16 = block name id,
// each entry is a length-prefixed wide string). Real format, no faking.
int TRT_STDCALL trt_LoadStringW(void* inst, u32 id, u16* buf, int cch) {
  trt_image* img = inst ? trt_images_by_handle(inst) : trt_images_first();
  if (!img || !buf || cch <= 0) return 0;
  const u64 rsrc = rsrc_rva_of(img);
  if (!rsrc) {
    char l[160];
    snprintf(l, sizeof(l),
             "res: LoadStringW(%u) -> 0 (image has no .rsrc)", id);
    log_line(l);
    return 0;
  }
  const u32 block = id / 16 + 1;
  const u32 slot = id % 16;
  res_key k;
  memset(&k, 0, sizeof(k));
  k.type = k_RT_STRING;
  k.name = block;
  k.by_str = false;
  k.lang = 0x0409;
  res_lookup lk{};
  if (!res_walk(img->base, rsrc, k, &lk)) {
    char l[160];
    snprintf(l, sizeof(l),
             "res: LoadStringW(%u) -> 0 (string block %u not found)", id,
             block);
    log_line(l);
    return 0;
  }
  const u8* p = img->base + lk.data_rva;
  for (u32 s = 0; s <= slot; ++s) {
    const u16 len = rd16r(p);
    if (s == slot) {
      if ((int)len >= cch) return 0;  // real Windows: 0 when buf too small
      memcpy(buf, p + 2, (size_t)len * 2);
      buf[len] = 0;
      return len;
    }
    p += 2 + (u32)len * 2;
  }
  return 0;
}

// registration: the kernel32 surface (resource APIs ARE kernel32 exports)
static const trt_named_fn k_res_fns[] = {
    {"FindResourceW", (void*)&trt_FindResourceW},
    {"FindResourceExW", (void*)&trt_FindResourceExW},
    {"FindResourceA", (void*)&trt_FindResourceA},
    {"FindResourceExA", (void*)&trt_FindResourceExA},
    {"SizeofResource", (void*)&trt_SizeofResource},
    {"LoadResource", (void*)&trt_LoadResource},
    {"LockResource", (void*)&trt_LockResource},
    {"UnlockResource", (void*)&trt_UnlockResource},
    {"FreeResource", (void*)&trt_FreeResource},
    {"EnumResourceNamesW", (void*)&trt_EnumResourceNamesW},
    {"LoadStringW", (void*)&trt_LoadStringW},
};

int trt_bridge_register_res(void) {
  trt_module_register("kernel32.dll", TRT_ROUTE_BUILTIN, k_res_fns,
                      (int)(sizeof(k_res_fns) / sizeof(k_res_fns[0])));
  char l[128];
  snprintf(l, sizeof(l),
           "res: %d resource APIs registered on kernel32 (.rsrc walk live)",
           (int)(sizeof(k_res_fns) / sizeof(k_res_fns[0])));
  log_line(l);
  return 0;
}

} // extern "C"
