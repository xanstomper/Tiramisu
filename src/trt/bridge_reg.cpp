// tiramisu/src/trt/bridge_reg.cpp
// A virtual registry for the guest (see bridge_reg.h).
//
// Shape:
//   HKEY roots        HKCR / HKCU / HKLM / HKU / HKCC (Windows' fixed values)
//   key nodes         name + parent + child list + value list
//   value nodes       name + type + bytes
//
// Persistence: a flat text hive on the guest's C: drive, rewritten after
// every write (the hive is small; games write a handful of keys).
//
// Handle model: the root keys use Windows' real handle values (0x80000000
// + index), opened keys get allocated handles from the same table, so the
// guest can pass them back like real HKEYs.
#include "bridge_reg.h"
#include "bridge_crypt32.h"
#include "thunk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

// ---- registry error codes the guest checks ----
constexpr int REG_SUCCESS = 0;
constexpr int REG_ERROR_FILE_NOT_FOUND = 2;
constexpr int REG_ERROR_ACCESS_DENIED = 5;
constexpr int REG_ERROR_INVALID_HANDLE = 6;
constexpr int REG_ERROR_NOT_ENOUGH_MEMORY = 8;
constexpr int REG_ERROR_NO_MORE_ITEMS = 259;
constexpr int REG_ERROR_MORE_DATA = 234;

// ---- value types ----
constexpr u32 TRT_REG_SZ = 1;
constexpr u32 TRT_REG_EXPAND_SZ = 2;
constexpr u32 TRT_REG_BINARY = 3;
constexpr u32 TRT_REG_DWORD = 4;
constexpr u32 TRT_REG_QWORD = 11;

struct reg_value {
  char name[128];
  u32 type;
  u32 size;
  u8* data;
  reg_value* next;
};

struct reg_key {
  char name[192];
  struct reg_key* parent;
  reg_key* child;
  reg_key* next_sibling;
  reg_value* values;
  reg_value* last_value;
  u32 child_count;
  u32 value_count;
  bool is_root;
};

#define REG_MAX_KEYS 256
#define REG_MAX_VALUES 512
#define REG_MAX_HANDLES 128

reg_key g_roots[5];
const char* g_root_names[5] = {"HKEY_CLASSES_ROOT", "HKEY_CURRENT_USER",
                                "HKEY_LOCAL_MACHINE", "HKEY_USERS",
                                "HKEY_CURRENT_CONFIG"};
// Windows' root handle values
const u32 g_root_handles[5] = {0x80000000u, 0x80000001u, 0x80000002u,
                               0x80000003u, 0x80000004u};

reg_key* g_open_keys[REG_MAX_HANDLES];
u32 g_handle_count = 0;
// the root handles are 0x80000000..0x80000004 (Windows' fixed values), so
// opened keys must start above them or a handle would mean two things
constexpr u32 REG_FIRST_HANDLE = 0x80000100u;

u16 g_hive_path[260] = {0};
bool g_hive_loaded = false;
bool g_hive_dirty = false;

// ------------------------------------------------------------------ helpers

reg_key* find_child(reg_key* parent, const char* name) {
  for (reg_key* k = parent->child; k; k = k->next_sibling)
    if (strcmp(k->name, name) == 0) return k;
  return nullptr;
}

reg_key* create_child(reg_key* parent, const char* name) {
  reg_key* k = (reg_key*)calloc(1, sizeof(reg_key));
  if (!k) return nullptr;
  snprintf(k->name, sizeof(k->name), "%s", name);
  k->parent = parent;
  k->next_sibling = parent->child;
  parent->child = k;
  ++parent->child_count;
  return k;
}

reg_value* find_value(reg_key* key, const char* name) {
  for (reg_value* v = key->values; v; v = v->next)
    if (strcmp(v->name, name) == 0) return v;
  return nullptr;
}

// split a buffer on a separator into fields (portable: no strtok_r/strtok_s)
int split_fields(char* line, char** fields, int max_fields, char sep) {
  int n = 0;
  char* p = line;
  while (n < max_fields) {
    fields[n++] = p;
    char* s = strchr(p, sep);
    if (!s) break;
    *s = 0;
    p = s + 1;
  }
  for (int i = 0; i < n; ++i) {
    size_t l = strlen(fields[i]);
    while (l && (fields[i][l - 1] == '\n' || fields[i][l - 1] == '\r'))
      fields[i][--l] = 0;
  }
  return n;
}

// path lookup: "Software\Foo\Bar" under a root
reg_key* open_path(reg_key* root, const char* path, bool create) {
  reg_key* cur = root;
  char buf[256];
  snprintf(buf, sizeof(buf), "%s", path);
  char* fields[32];
  const int n = split_fields(buf, fields, 32, '\\');
  for (int fi = 0; fi < n; ++fi) {
    char* tok = fields[fi];
    if (!*tok) continue;
    reg_key* next = find_child(cur, tok);
    if (!next) {
      if (!create) return nullptr;
      next = create_child(cur, tok);
      if (!next) return nullptr;
    }
    cur = next;
  }
  return cur;
}

reg_key* root_for_handle(u64 h) {
  // Win64 predefined HKEYs are signed LONG constants widened to pointers.
  // Also accept the zero-extended form used by 32-bit guests.
  for (int i = 0; i < 5; ++i) {
    const u64 root = g_root_handles[i];
    if (h == root || h == (0xffffffff00000000ULL | root))
      return &g_roots[i];
  }
  return nullptr;
}

reg_key* key_for_handle(u64 h) {
  reg_key* root = root_for_handle(h);
  if (root) return root;
  if (h >= REG_FIRST_HANDLE && h < REG_FIRST_HANDLE + REG_MAX_HANDLES)
    return g_open_keys[h - REG_FIRST_HANDLE];
  return nullptr;
}

u64 allocate_handle(reg_key* key) {
  for (u32 i = 0; i < g_handle_count; ++i) {
    if (g_open_keys[i]) continue;
    g_open_keys[i] = key;
    return REG_FIRST_HANDLE + i;
  }
  if (g_handle_count >= REG_MAX_HANDLES) return 0;
  g_open_keys[g_handle_count] = key;
  return REG_FIRST_HANDLE + g_handle_count++;
}

// ------------------------------------------------------------------ storage

void set_value(reg_key* key, const char* name, u32 type, const void* data,
               u32 size) {
  reg_value* v = find_value(key, name);
  if (!v) {
    v = (reg_value*)calloc(1, sizeof(reg_value));
    if (!v) return;
    snprintf(v->name, sizeof(v->name), "%s", name);
    v->next = nullptr;
    if (key->last_value) key->last_value->next = v;
    else key->values = v;
    key->last_value = v;
    ++key->value_count;
  }
  u8* copy = (u8*)malloc(size ? size : 1);
  if (!copy) return;
  if (data && size) memcpy(copy, data, size);
  free(v->data);
  v->data = copy;
  v->type = type;
  v->size = size;
  g_hive_dirty = true;
}

void delete_value(reg_key* key, const char* name) {
  reg_value* prev = nullptr;
  for (reg_value* v = key->values; v; prev = v, v = v->next) {
    if (strcmp(v->name, name) != 0) continue;
    if (prev) prev->next = v->next;
    else key->values = v->next;
    if (key->last_value == v) key->last_value = prev;
    free(v->data);
    free(v);
    --key->value_count;
    g_hive_dirty = true;
    return;
  }
}

// ------------------------------------------------------------- persistence

void host_path_of_hive(char* out, size_t cap) {
  out[0] = 0;
  if (!g_hive_path[0]) return;
  if (trt_vfs_resolve_utf16(g_hive_path, out, cap) != 0) return;
}

// full path of a key under its root ("Software\\Tiramisu", "" for a root)
bool key_path(reg_key* k, char* out, size_t cap) {
  const char* parts[16];
  int n = 0;
  for (reg_key* p = k; p && !p->is_root && n < 16; p = p->parent)
    parts[n++] = p->name;
  out[0] = 0;
  for (int j = n - 1; j >= 0; --j) {
    if (out[0]) strncat(out, "\\", cap - strlen(out) - 1);
    strncat(out, parts[j], cap - strlen(out) - 1);
  }
  return true;
}

// hex-encode everything that is not a plain number, so UTF-16 strings and
// binary blobs survive the round trip
void hex_encode(const u8* data, u32 size, char* out, size_t cap) {
  size_t o = 0;
  for (u32 i = 0; i < size && o + 2 < cap; ++i) {
    snprintf(out + o, cap - o, "%02x", data[i]);
    o += 2;
  }
  out[o] = 0;
}

bool hex_decode(const char* hex, u8* out, size_t cap, u32* size) {
  const size_t len = strlen(hex);
  if (len % 2) return false;
  u32 n = 0;
  for (size_t i = 0; i + 1 < len; i += 2) {
    if (n + 1 > cap) return false;
    char pair[3] = {hex[i], hex[i + 1], 0};
    out[n++] = (u8)strtoul(pair, nullptr, 16);
  }
  *size = n;
  return true;
}

void write_hive() {
  char host[512];
  host_path_of_hive(host, sizeof(host));
  if (!host[0]) return;
  FILE* f = fopen(host, "wb");
  if (!f) return;
  fprintf(f, "# tiramisu registry hive v1\n");
  for (int i = 0; i < 5; ++i) {
    reg_key* stack[64];
    int sp = 0;
    stack[sp++] = &g_roots[i];
    while (sp > 0) {
      reg_key* k = stack[--sp];
      for (reg_key* c = k->child; c; c = c->next_sibling)
        if (sp < 64) stack[sp++] = c;
      if (!k->values) continue;
      char path[512];
      key_path(k, path, sizeof(path));
      for (reg_value* v = k->values; v; v = v->next) {
        char name[256], data[2048];
        hex_encode(v->data, v->size, data, sizeof(data));
        snprintf(name, sizeof(name), "%s", v->name);
        fprintf(f, "k\t%s\t%s\t%s\t%u\t%s\n", g_root_names[i], path, name,
                v->type, data);
      }
    }
  }
  fclose(f);
  g_hive_dirty = false;
}

// ---- well-known key seeds ------------------------------------------------
// Games probe a few keys at startup and treat FILE_NOT_FOUND as "Steam /
// DirectX / Windows is broken" (or worse, as a signal to install things).
// Seed them with plausible values; the persisted hive (loaded after this)
// overwrites any seed the user or a game already set for real, and a seed
// never overwrites a value already present.
void seed_sz(reg_key* key, const char* name, const char* utf8) {
  if (!key || find_value(key, name)) return;
  // REG_SZ is stored as raw UTF-16LE bytes incl. the terminating NUL
  u16 w[256];
  size_t n = 0;
  for (; utf8[n] && n + 1 < sizeof(w) / sizeof(w[0]); ++n)
    w[n] = (u16)(u8)utf8[n];
  w[n] = 0;
  set_value(key, name, TRT_REG_SZ, w, (u32)((n + 1) * 2));
}

void seed_binary(reg_key* key, const char* name, const u8* data, u32 size) {
  if (!key || find_value(key, name)) return;
  set_value(key, name, TRT_REG_BINARY, data, size);
}

void seed_registry() {
  reg_key* hklm = &g_roots[2];  // HKEY_LOCAL_MACHINE
  // Valve\Steam: the install path every Steamworks game probes first
  reg_key* steam = open_path(hklm, "SOFTWARE\\Valve\\Steam", true);
  seed_sz(steam, "InstallPath", "C:\\Program Files (x86)\\Steam");
  seed_sz(steam, "SteamPath", "C:\\Program Files (x86)\\Steam");
  seed_sz(steam, "Language", "english");
  // Windows\CurrentVersion: matches the GetVersionExW personality (10.0.19045)
  reg_key* cv =
      open_path(hklm, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion", true);
  seed_sz(cv, "ProductName", "Windows 10 Pro");
  seed_sz(cv, "CurrentVersion", "6.3");
  seed_sz(cv, "CurrentBuild", "19045");
  seed_sz(cv, "DisplayVersion", "22H2");
  seed_sz(cv, "ProgramFilesDir", "C:\\Program Files");
  seed_sz(cv, "ProgramFilesDir (x86)", "C:\\Program Files (x86)");
  seed_sz(cv, "CommonFilesDir", "C:\\Program Files\\Common Files");
  seed_sz(cv, "CommonFilesDir (x86)", "C:\\Program Files (x86)\\Common Files");
  // DirectX: the 9.0c version stamp D3D9-era engines check
  reg_key* dx = open_path(hklm, "SOFTWARE\\Microsoft\\DirectX", true);
  seed_sz(dx, "Version", "4.09.00.0904");
  const u8 installed_version[8] = {0, 0, 0, 9, 0, 0, 0, 0};
  seed_binary(dx, "InstalledVersion", installed_version,
              sizeof(installed_version));
}

void load_hive() {
  if (g_hive_loaded) return;
  g_hive_loaded = true;
  for (int i = 0; i < 5; ++i) {
    memset(&g_roots[i], 0, sizeof(g_roots[i]));
    snprintf(g_roots[i].name, sizeof(g_roots[i].name), "%s", g_root_names[i]);
    g_roots[i].is_root = true;
  }
  seed_registry();  // before the persisted hive: real values win over seeds
  char host[512];
  host_path_of_hive(host, sizeof(host));
  if (!host[0]) return;
  FILE* f = fopen(host, "rb");
  if (!f) return;
  char line[4096];
  while (fgets(line, sizeof(line), f)) {
    if (line[0] == '#' || line[0] == '\n') continue;
    char* f[8];
    const int nf = split_fields(line, f, 8, '\t');
    if (nf < 6) continue;
    if (strcmp(f[0], "k") != 0) continue;
    const char* root_name = f[1];
    const char* path = f[2];
    const char* vname = f[3];
    const char* type_s = f[4];
    const char* data_s = f[5];
    if (!root_name || !path || !vname || !type_s || !data_s) continue;
    int ri = -1;
    for (int i = 0; i < 5; ++i)
      if (strcmp(g_root_names[i], root_name) == 0) ri = i;
    if (ri < 0) continue;
    reg_key* key = open_path(&g_roots[ri], path, true);
    if (!key) continue;
    const u32 type = (u32)strtoul(type_s, nullptr, 10);
    u8 buf[2048];
    u32 size = 0;
    const size_t numeric_size = type == TRT_REG_DWORD ? sizeof(u32)
                              : type == TRT_REG_QWORD ? sizeof(u64) : 0;
    // write_hive stores numeric values as little-endian hex bytes too.
    // Continue accepting older decimal entries without the full hex width.
    if (numeric_size && strlen(data_s) != numeric_size * 2) {
      char* end = nullptr;
      const u64 number = strtoull(data_s, &end, 10);
      if (end == data_s || *end != '\0') continue;
      size = static_cast<u32>(numeric_size);
      memcpy(buf, &number, size);
    } else if (!hex_decode(data_s, buf, sizeof(buf), &size)) {
      continue;
    }
    set_value(key, vname, type, buf, size);
  }
  fclose(f);
  g_hive_dirty = false;
}

// ------------------------------------------------------------- the functions

u32 reg_strlen(const u16* s) {
  u32 n = 0;
  while (s && s[n]) ++n;
  return n;
}

void reg_to_utf8(const u16* in, char* out, size_t cap) {
  size_t o = 0;
  for (size_t i = 0; in && in[i] && o + 1 < cap; ++i) {
    if (in[i] < 0x80) out[o++] = (char)in[i];
    else if (in[i] < 0x800) {
      if (o + 2 >= cap) break;
      out[o++] = (char)(0xC0 | (in[i] >> 6));
      out[o++] = (char)(0x80 | (in[i] & 0x3F));
    } else {
      if (o + 3 >= cap) break;
      out[o++] = (char)(0xE0 | (in[i] >> 12));
      out[o++] = (char)(0x80 | ((in[i] >> 6) & 0x3F));
      out[o++] = (char)(0x80 | (in[i] & 0x3F));
    }
  }
  out[o] = 0;
}

} // namespace

extern "C" {

void trt_registry_set_hive_path(const u16* guest_path) {
  for (int i = 0; i < 260 && guest_path && guest_path[i]; ++i)
    g_hive_path[i] = guest_path[i];
  g_hive_loaded = false;
}

int TRT_GUEST_ABI trt_RegOpenKeyExW(u64 hkey, const u16* subkey, u32 options,
                                    u32 sam, void** out) {
  (void)options; (void)sam;
  load_hive();
  reg_key* root = key_for_handle(hkey);
  if (!root || !out) return REG_ERROR_INVALID_HANDLE;
  char path[256];
  reg_to_utf8(subkey, path, sizeof(path));
  reg_key* key = path[0] ? open_path(root, path, false) : root;
  if (!key) {
    // Wine/Windows: opening a missing key with KEY_WOW64_* etc. still fails
    return REG_ERROR_FILE_NOT_FOUND;
  }
  const u64 handle = allocate_handle(key);
  if (!handle) return REG_ERROR_NOT_ENOUGH_MEMORY;
  *out = reinterpret_cast<void*>(static_cast<uintptr_t>(handle));
  return REG_SUCCESS;
}

int TRT_GUEST_ABI trt_RegCreateKeyExW(u64 hkey, const u16* subkey, u32 reserved,
                                       void* cls, u32 options, u32 sam,
                                       void** out, u32* disposition) {
  (void)reserved; (void)cls; (void)options; (void)sam;
  load_hive();
  reg_key* root = key_for_handle(hkey);
  if (!root || !out) return REG_ERROR_INVALID_HANDLE;
  char path[256];
  reg_to_utf8(subkey, path, sizeof(path));
  if (g_handle_count == REG_MAX_HANDLES) {
    bool has_free_slot = false;
    for (u32 i = 0; i < g_handle_count; ++i) {
      if (!g_open_keys[i]) { has_free_slot = true; break; }
    }
    if (!has_free_slot) return REG_ERROR_NOT_ENOUGH_MEMORY;
  }
  reg_key* existing = path[0] ? open_path(root, path, false) : root;
  reg_key* key = existing;
  if (!key) {
    key = open_path(root, path, true);
    if (!key) return REG_ERROR_ACCESS_DENIED;
    if (disposition) *disposition = 1;  // REG_CREATED_NEW_KEY
    g_hive_dirty = true;
  } else if (disposition) {
    *disposition = 2;  // REG_OPENED_EXISTING_KEY
  }
  const u64 handle = allocate_handle(key);
  if (!handle) return REG_ERROR_NOT_ENOUGH_MEMORY;
  *out = reinterpret_cast<void*>(static_cast<uintptr_t>(handle));
  write_hive();
  return REG_SUCCESS;
}

int TRT_GUEST_ABI trt_RegQueryValueExW(u64 hkey, const u16* name, u32* reserved,
                                       u32* type, u8* data, u32* lpcbData) {
  (void)reserved;
  load_hive();
  reg_key* key = key_for_handle(hkey);
  char vname[128];
  reg_to_utf8(name, vname, sizeof(vname));
  char line[256];
  snprintf(line, sizeof(line), "trt: RegQueryValueExW(hkey=0x%llx, '%s') key=%p",
           (unsigned long long)hkey, vname, (void*)key);
  trt_core_log(line);
  if (!key) return REG_ERROR_INVALID_HANDLE;
  reg_value* v = find_value(key, vname);
  if (!v) { char st_line[256]; snprintf(st_line, sizeof(st_line), "strtrace: RegQueryValueExW -> name=\"%.64s\" value=NULL (not found)", vname); trt_core_log(st_line); return REG_ERROR_FILE_NOT_FOUND; }
  if (type) *type = v->type;
  const u32 capacity = lpcbData && data ? *lpcbData : 0;
  if (lpcbData) *lpcbData = v->size;
  if (!data) return REG_SUCCESS;
  if (capacity < v->size) return REG_ERROR_MORE_DATA;
  memcpy(data, v->data, v->size);
  { char st_tmp[136]; char st_line[256]; if (v->type == TRT_REG_SZ || v->type == TRT_REG_EXPAND_SZ) { const u16* st_w = (const u16*)v->data; size_t st_n = v->size / 2; if (st_n > 132) st_n = 132; size_t st_i = 0; for (; st_i < st_n && st_w[st_i]; ++st_i) st_tmp[st_i] = (char)(st_w[st_i] & 0x7f); st_tmp[st_i] = 0; snprintf(st_line, sizeof(st_line), "strtrace: RegQueryValueExW -> name=\"%.64s\" type=%u value=\"%.128s\"", vname, v->type, st_tmp); } else { snprintf(st_line, sizeof(st_line), "strtrace: RegQueryValueExW -> name=\"%.64s\" type=%u size=%u (non-string)", vname, v->type, v->size); } trt_core_log(st_line); }
  return REG_SUCCESS;
}

int TRT_GUEST_ABI trt_RegSetValueExW(u64 hkey, const u16* name, u32 reserved,
                                      u32 type, const u8* data, u32 cbData) {
  (void)reserved;
  load_hive();
  reg_key* key = key_for_handle(hkey);
  if (!key) return REG_ERROR_INVALID_HANDLE;
  char vname[128];
  reg_to_utf8(name, vname, sizeof(vname));
  set_value(key, vname, type, data, cbData);
  write_hive();
  return REG_SUCCESS;
}

int TRT_GUEST_ABI trt_RegDeleteValueW(u64 hkey, const u16* name) {
  load_hive();
  reg_key* key = key_for_handle(hkey);
  if (!key) return REG_ERROR_INVALID_HANDLE;
  char vname[128];
  reg_to_utf8(name, vname, sizeof(vname));
  if (!find_value(key, vname)) return REG_ERROR_FILE_NOT_FOUND;
  delete_value(key, vname);
  write_hive();
  return REG_SUCCESS;
}

void free_subtree(reg_key* key) {
  if (!key || key->is_root) return;
  while (key->child) {
    free_subtree(key->child);
  }
  if (key->parent) {
    reg_key* prev = nullptr;
    for (reg_key* c = key->parent->child; c; prev = c, c = c->next_sibling) {
      if (c == key) {
        if (prev) prev->next_sibling = c->next_sibling;
        else key->parent->child = c->next_sibling;
        --key->parent->child_count;
        break;
      }
    }
  }
  while (key->values) {
    reg_value* next = key->values->next;
    free(key->values->data);
    free(key->values);
    key->values = next;
  }
  for (u32 i = 0; i < g_handle_count; ++i) {
    if (g_open_keys[i] == key) g_open_keys[i] = nullptr;
  }
  free(key);
  g_hive_dirty = true;
}

int TRT_GUEST_ABI trt_RegDeleteKeyExW(u64 hkey, const u16* subkey, u32 sam,
                                      u32 reserved) {
  (void)sam; (void)reserved;
  load_hive();
  reg_key* root = key_for_handle(hkey);
  if (!root || !subkey || !subkey[0]) return REG_ERROR_INVALID_HANDLE;
  char path[256];
  reg_to_utf8(subkey, path, sizeof(path));
  reg_key* target = open_path(root, path, false);
  if (!target || target->is_root) return REG_ERROR_FILE_NOT_FOUND;
  if (target->child != nullptr) return REG_ERROR_ACCESS_DENIED;
  free_subtree(target);
  write_hive();
  return REG_SUCCESS;
}

int TRT_GUEST_ABI trt_RegDeleteKeyW(u64 hkey, const u16* subkey) {
  return trt_RegDeleteKeyExW(hkey, subkey, 0, 0);
}

int TRT_GUEST_ABI trt_RegDeleteTreeW(u64 hkey, const u16* subkey) {
  load_hive();
  reg_key* root = key_for_handle(hkey);
  if (!root) return REG_ERROR_INVALID_HANDLE;
  if (!subkey || !subkey[0]) {
    if (root->is_root) return REG_ERROR_ACCESS_DENIED;
    while (root->child) free_subtree(root->child);
    while (root->values) delete_value(root, root->values->name);
    write_hive();
    return REG_SUCCESS;
  }
  char path[256];
  reg_to_utf8(subkey, path, sizeof(path));
  reg_key* target = open_path(root, path, false);
  if (!target) return REG_ERROR_FILE_NOT_FOUND;
  if (target->is_root) return REG_ERROR_ACCESS_DENIED;
  free_subtree(target);
  write_hive();
  return REG_SUCCESS;
}

int TRT_GUEST_ABI trt_RegFlushKey(u64 hkey) {
  if (!key_for_handle(hkey)) return REG_ERROR_INVALID_HANDLE;
  if (g_hive_dirty) write_hive();
  return REG_SUCCESS;
}

int TRT_GUEST_ABI trt_RegOpenCurrentUser(u32 sam, u64* phk) {
  (void)sam;
  if (!phk) return 87;
  load_hive();
  *phk = allocate_handle(&g_roots[1]);  // HKCU
  return *phk ? REG_SUCCESS : REG_ERROR_NOT_ENOUGH_MEMORY;
}

int TRT_GUEST_ABI trt_RegConnectRegistryW(const u16* machine, u64 hkey, u64* phk) {
  if (!phk) return 87;
  if (machine && machine[0]) return 53;  // ERROR_BAD_NETPATH
  load_hive();
  reg_key* k = key_for_handle(hkey);
  if (!k) return REG_ERROR_INVALID_HANDLE;
  *phk = allocate_handle(k);
  return *phk ? REG_SUCCESS : REG_ERROR_NOT_ENOUGH_MEMORY;
}

int TRT_GUEST_ABI trt_RegSaveKeyW(u64 hkey, const u16* file, void* sa) {
  (void)hkey; (void)file; (void)sa;
  return REG_ERROR_ACCESS_DENIED;
}
int TRT_GUEST_ABI trt_RegLoadKeyW(u64 hkey, const u16* subkey, const u16* file) {
  (void)hkey; (void)subkey; (void)file;
  return REG_ERROR_ACCESS_DENIED;
}

int TRT_GUEST_ABI trt_RegCloseKey(u64 hkey) {
  if (root_for_handle(hkey)) return REG_SUCCESS;
  if (hkey < REG_FIRST_HANDLE || hkey - REG_FIRST_HANDLE >= g_handle_count)
    return REG_ERROR_INVALID_HANDLE;
  reg_key*& slot = g_open_keys[hkey - REG_FIRST_HANDLE];
  if (!slot) return REG_ERROR_INVALID_HANDLE;
  slot = nullptr;
  return REG_SUCCESS;
}
int TRT_GUEST_ABI trt_RegEnumKeyExW(u64 hkey, u32 index, u16* name,
                                     u32 cchMax, u32* reserved,
                                     u32* lpcchName, u32* last_write) {
  (void)reserved; (void)last_write;
  load_hive();
  reg_key* key = key_for_handle(hkey);
  if (!key) return REG_ERROR_INVALID_HANDLE;
  u32 n = 0;
  for (reg_key* c = key->child; c; c = c->next_sibling) {
    if (n == index) {
      const u32 len = (u32)strlen(c->name) + 1;
      if (lpcchName) *lpcchName = len;
      if (!name) return REG_SUCCESS;
      if (cchMax < len) return REG_ERROR_MORE_DATA;
      for (u32 i = 0; i < len; ++i) name[i] = (u16)(u8)c->name[i];
      return REG_SUCCESS;
    }
    ++n;
  }
  return REG_ERROR_NO_MORE_ITEMS;
}

int TRT_GUEST_ABI trt_RegEnumValueW(u64 hkey, u32 index, u16* name, u32 cchMax,
                                     u32* reserved, u32* lpcchName,
                                     u32* last_write, u32* type, u8* data,
                                     u32* cbData) {
  (void)reserved; (void)last_write;
  load_hive();
  reg_key* key = key_for_handle(hkey);
  if (!key) return REG_ERROR_INVALID_HANDLE;
  u32 n = 0;
  for (reg_value* v = key->values; v; v = v->next) {
    if (n == index) {
      const u32 len = (u32)strlen(v->name) + 1;
      if (lpcchName) *lpcchName = len;
      if (type) *type = v->type;
      if (cbData) *cbData = v->size;
      if (!name) return REG_SUCCESS;
      if (cchMax < len) return REG_ERROR_MORE_DATA;
      for (u32 i = 0; i < len; ++i) name[i] = (u16)(u8)v->name[i];
      if (data && cbData && *cbData >= v->size) memcpy(data, v->data, v->size);
      return REG_SUCCESS;
    }
    ++n;
  }
  return REG_ERROR_NO_MORE_ITEMS;
}

// modern API: RegGetValueW(HKEY, subkey, value, flags, type, data, cb, cbOut)
int TRT_GUEST_ABI trt_RegGetValueW(u64 hkey, const u16* subkey, const u16* value,
                                    u32 flags, u32* type, u8* data, u32 cbData,
                                    u32* cbDataOut) {
  (void)flags;
  load_hive();
  reg_key* root = key_for_handle(hkey);
  if (!root) return REG_ERROR_INVALID_HANDLE;
  char path[256];
  reg_to_utf8(subkey, path, sizeof(path));
  reg_key* key = path[0] ? open_path(root, path, false) : root;
  if (!key) return REG_ERROR_FILE_NOT_FOUND;
  char vname[128];
  reg_to_utf8(value, vname, sizeof(vname));
  reg_value* v = find_value(key, vname);
  if (!v) return REG_ERROR_FILE_NOT_FOUND;
  if (type) *type = v->type;
  if (cbDataOut) *cbDataOut = v->size;
  if (data && cbData >= v->size) memcpy(data, v->data, v->size);
  return REG_SUCCESS;
}

// ============================ token / security ==============================
// OrTMM (and many PC games) import advapi32's token surface at boot to ask
// "who am I". The honest answers on the console:
//   OpenProcessToken: a REAL pseudo token handle (a record here — never a
//     fake NULL or a fake failure: UWP processes DO have a process token).
//   GetTokenInformation: TokenUser / TokenIntegrityLevel / TokenStatistics
//     get minimal real records; anything else fails ERROR_INVALID_PARAMETER
//     (named).
//   ConvertStringSecurityDescriptorToSecurityDescriptorW: the console has
//     no SDDL security: fail ERROR_CALL_NOT_IMPLEMENTED, named.
//   ConvertSidToStringSidW: convert a real SID record if the bytes parse;
//     fail ERROR_INVALID_PARAMETER otherwise.
namespace {

// token records: OpenProcessToken hands these out
constexpr u32 TOKEN_HANDLE_TAG = 0x544B0000u;  // 'TK'
bool g_token_used[8];

bool token_is_valid(u32 h) {
  if ((h & 0xFFFF0000u) != TOKEN_HANDLE_TAG) return false;
  const u32 idx = h & 0xFFFFu;
  return idx > 0 && idx <= 8 && g_token_used[idx - 1];
}

// token access rights
constexpr u32 TRT_TOKEN_QUERY_RIGHT = 0x0008;

// TOKEN_INFORMATION classes
constexpr u32 TRT_TOKENINFO_USER = 1;
constexpr u32 TRT_TOKENINFO_STATISTICS = 10;
constexpr u32 TOKEN_INTEGRITY_LEVEL = 25;
// TokenAppContainerSid (winnt.h): PyInstaller queries this to decide whether
// to add an AppContainer ACE to its temp-directory DACL. Honest answer: the
// console guest is NOT running in an AppContainer, so the record is an 8-byte
// TOKEN_APPCONTAINER_INFORMATION { PSID TokenAppContainer = NULL }.
constexpr u32 TOKEN_APPCONTAINER_SID = 31;

// the pseudo user SID: S-1-5-88-1-1001 (a domain-less local player).
// SID layout: Revision u8, SubAuthorityCount u8, IdentifierAuthority
// (6 bytes BE, 0x000000000005 = NT authority), SubAuthority[] u32 LE.
constexpr u8 k_sid_bytes[24] = {
    1,                 // revision
    3,                 // sub-authority count (88, 1, 1001)
    0, 0, 0, 0, 0, 5,  // ID authority: NT
    88, 0, 0, 0,       // 88
    1, 0, 0, 0,        // 1
    233, 3, 0, 0,      // 1001 (the RID)
};

// the integrity SID: S-1-16-8192 (medium integrity)
constexpr u8 k_il_sid[12] = {1, 1, 0, 0, 0, 0, 0, 16, 0, 32, 0, 0};

constexpr u32 k_token_user_size = 36;    // SID_AND_ATTRIBUTES(12) + SID(24)
constexpr u32 k_token_il_size = 40;      // SID_AND_ATTRIBUTES(12) + SID(12) + mask room
constexpr u32 k_token_stats_size = 72;   // LUIDs + type/level fields
constexpr u32 k_token_appcontainer_size = 8;  // { PSID } = NULL (no AppContainer)

// thread-local last error lives in bridge_kernel32.cpp; use the exported
// setter so guest GetLastError() sees our named failures
void token_set_last_error(u32 err);

} // namespace

extern "C" u32 TRT_STDCALL trt_SetLastError(u32 err);  // bridge_kernel32

namespace {
void token_set_last_error(u32 err) { trt_SetLastError(err); }
} // namespace

} // extern "C"  (registry half)

extern "C" {  // token/security half

// ============================ token / security ==============================

void* TRT_GUEST_ABI trt_OpenProcessToken(void* process, u32 desired,
                                          void* token_out) {
  (void)process;  // GetCurrentProcess pseudo handle is fine
  if (!token_out) {
    token_set_last_error(87);  // ERROR_INVALID_PARAMETER
    return 0;
  }
  if ((desired & (TRT_TOKEN_QUERY_RIGHT | 0x0020u)) == 0) {
    // we only support QUERY / ADJUST_PRIVILEGES; anything else fails honestly
    char line[160];
    snprintf(line, sizeof(line),
             "advapi32: OpenProcessToken desired=0x%x (no QUERY/ADJUST bit) -> "
             "ERROR_ACCESS_DENIED", desired);
    trt_core_log(line);
    token_set_last_error(5);  // ERROR_ACCESS_DENIED
    return 0;
  }
  int slot = -1;
  for (int i = 0; i < 8; ++i)
    if (!g_token_used[i]) { slot = i; break; }
  if (slot < 0) {
    token_set_last_error(5);
    return 0;
  }
  g_token_used[slot] = true;
  *(void**)token_out = (void*)(uintptr_t)(TOKEN_HANDLE_TAG | (u32)(slot + 1));
  char line[128];
  snprintf(line, sizeof(line),
           "advapi32: OpenProcessToken -> token record (pseudo handle)");
  trt_core_log(line);
  return (void*)(uintptr_t)1;  // TRUE
}

int TRT_GUEST_ABI trt_GetTokenInformation(void* token, u32 cls,
                                           void* info, u32 len,
                                           u32* len_out) {
  if (!token_is_valid((u32)(uintptr_t)token)) {
    char line[160];
    snprintf(line, sizeof(line),
             "advapi32: GetTokenInformation bad token handle 0x%x -> "
             "ERROR_INVALID_HANDLE", (u32)(uintptr_t)token);
    trt_core_log(line);
    token_set_last_error(6);  // ERROR_INVALID_HANDLE
    return 0;
  }
  // the classes games and tooling actually ask, with minimal REAL records
  u32 fill = 0;
  switch (cls) {
    case TRT_TOKENINFO_USER: fill = k_token_user_size; break;
    case TOKEN_INTEGRITY_LEVEL: fill = k_token_il_size; break;
    case TRT_TOKENINFO_STATISTICS: fill = k_token_stats_size; break;
    case TOKEN_APPCONTAINER_SID: fill = k_token_appcontainer_size; break;
    default: {
      char line[160];
      snprintf(line, sizeof(line),
               "advapi32: GetTokenInformation class=%u unsupported -> "
               "ERROR_INVALID_PARAMETER (named, fail-visible)", cls);
      trt_core_log(line);
      token_set_last_error(87);
      return 0;
    }
  }
  // size-query form: info == NULL with len == 0 is the canonical
  // "how big is this record?" probe (PyInstaller's bootloader does exactly
  // this before allocating). Answer with the size + ERROR_INSUFFICIENT_BUFFER,
  // the documented contract — NOT ERROR_INVALID_PARAMETER.
  if (!info) {
    if (len_out) *len_out = fill;
    token_set_last_error(122);  // ERROR_INSUFFICIENT_BUFFER
    return 0;
  }
  if (!len_out) {
    token_set_last_error(87);
    return 0;
  }
  if (len < fill) {
    char line[160];
    snprintf(line, sizeof(line),
             "advapi32: GetTokenInformation class=%u buffer=%u need=%u -> "
             "ERROR_INSUFFICIENT_BUFFER", cls, len, fill);
    trt_core_log(line);
    *len_out = fill;
    token_set_last_error(122);  // ERROR_INSUFFICIENT_BUFFER
    return 0;
  }
  *len_out = fill;
  u8* out = (u8*)info;
  memset(out, 0, fill);
  if (cls == TRT_TOKENINFO_USER) {
    // SID_AND_ATTRIBUTES { pSid, u32 attrs }: pSid points INTO the
    // caller's buffer (winnt semantics), the SID bytes follow the pointer.
    // The struct is { void*, u32 } with 4 bytes tail padding — never write
    // the u32 at +4, that would zero the pointer's upper half.
    u8* sid = out + 16;
    memcpy(sid, k_sid_bytes, sizeof(k_sid_bytes));
    *(void**)(out + 0) = sid;
    *(u32*)(out + 8) = 0;  // attributes
  } else if (cls == TOKEN_INTEGRITY_LEVEL) {
    u8* sid = out + 16;
    memcpy(sid, k_il_sid, sizeof(k_il_sid));
    *(void**)(out + 0) = sid;
    *(u32*)(out + 8) = 0;
    *(u32*)(out + 28) = 0;  // policy mask: none (AFTER the 12-byte SID)
  } else if (cls == TRT_TOKENINFO_STATISTICS) {  // minimal stats record
    *(u64*)(out + 0) = 0x2A;   // TokenId LUID (stable pseudo)
    *(u64*)(out + 8) = 0x2A;   // AuthenticationId
    *(u64*)(out + 16) = 0;     // ExpirationTime: never
    *(u32*)(out + 24) = 1;     // TokenType: TokenPrimary
    *(u32*)(out + 28) = 0;     // ImpersonationLevel: Anonymous
  } else if (cls == TOKEN_APPCONTAINER_SID) {
    // { PSID } = NULL: the console guest is not running in an AppContainer
    *(void**)(out + 0) = nullptr;
  }
  char line[128];
  snprintf(line, sizeof(line),
           "advapi32: GetTokenInformation class=%u -> %u bytes of record",
           cls, fill);
  trt_core_log(line);
  return 1;  // TRUE
}

// backend allocator singleton (images.cpp): the same pool HeapAlloc hands
// out of, so the guest's LocalFree on the SD releases it honestly.
const trt_backend* trt_runtime_backend(void);  // images.cpp

int TRT_GUEST_ABI trt_ConvertStringSecurityDescriptorToSecurityDescriptorW(
    const u16* sddl, u32 req, void* sd_out, void* len_out) {
  (void)req;  // SDDL_REVISION_1 is the only revision; accept it
  char utf8[128];
  if (sddl)
    trt_utf16_to_utf8(sddl, utf8, sizeof(utf8));
  else
    snprintf(utf8, sizeof(utf8), "(null)");

  // The console has no security model, but the guest may legitimately pass
  // the resulting SECURITY_DESCRIPTOR blob onward (PyInstaller hands it to
  // CreateDirectoryW and LocalFree). The honest-minimal surface is a REAL
  // self-relative SECURITY_DESCRIPTOR with a NULL DACL (everyone-full, the
  // on-disk truth of this runtime) so downstream consumers see well-formed
  // bytes, logged as such — never a claim that access is actually restricted.
  //
  // SDDL we support: "D:" followed by zero or more (A;;FA;;;SID) ACEs. We
  // count the ACEs only to log them; the DACL itself is NULL either way.
  unsigned n_ace = 0;
  for (const char* p = utf8; (p = strstr(p, "(A;")) != nullptr; p += 1) ++n_ace;

  // self-relative SD layout (winnt.h SECURITY_DESCRIPTOR_RELATIVE):
  //   u8  Revision; u8 SandboxInert-pad; u16 Control;
  //   u32 OwnerOffset; u32 GroupOffset; u32 SaclOffset; u32 DaclOffset;
  //   [Owner SID][Group SID]
  // SE_SELF_RELATIVE = 0x8000. Owner = the console pseudo user SID
  // (S-1-5-88-1-1001), Group = same SID (a valid, honest grouping).
  constexpr u32 k_se_self_relative = 0x8000;
  constexpr u32 k_hdr = 20;
  const u32 owner_off = k_hdr;
  const u32 group_off = owner_off + (u32)sizeof(k_sid_bytes);
  const u32 blob = group_off + (u32)sizeof(k_sid_bytes);
  u8* sd = trt_runtime_backend()
               ? (u8*)trt_runtime_backend()->alloc(blob, 0)
               : (u8*)malloc(blob);
  if (!sd) {
    token_set_last_error(8);  // ERROR_NOT_ENOUGH_MEMORY
    return 0;
  }
  memset(sd, 0, blob);
  sd[0] = 1;  // Revision
  *(u16*)(sd + 2) = (u16)k_se_self_relative;
  *(u32*)(sd + 4) = owner_off;
  *(u32*)(sd + 8) = group_off;
  *(u32*)(sd + 12) = 0;  // Sacl: none
  *(u32*)(sd + 16) = 0;  // Dacl: NULL DACL = allow all (the honest truth here)
  memcpy(sd + owner_off, k_sid_bytes, sizeof(k_sid_bytes));
  memcpy(sd + group_off, k_sid_bytes, sizeof(k_sid_bytes));
  if (sd_out) *(void**)sd_out = sd;
  if (len_out) *(u32*)len_out = blob;
  char line[288];
  snprintf(line, sizeof(line),
           "advapi32: ConvertStringSecurityDescriptorToSecurityDescriptorW"
           "('%s') -> self-relative SD %u bytes (NULL DACL = allow-all; "
           "no security model on console; %u ACE(s) noted, not enforced)",
           utf8, blob, n_ace);
  trt_core_log(line);
  return 1;  // TRUE
}

int TRT_GUEST_ABI trt_ConvertSidToStringSidW(const void* sid, u16** str_out) {
  if (!sid || !str_out) {
    token_set_last_error(87);
    return 0;
  }
  const u8* s = (const u8*)sid;
  if (s[0] != 1 /*revision*/ || s[1] == 0 || s[1] > 15) {
    char line[160];
    snprintf(line, sizeof(line),
             "advapi32: ConvertSidToStringSidW bad SID (rev=%u n=%u) -> "
             "ERROR_INVALID_PARAMETER", s[0], s[1]);
    trt_core_log(line);
    token_set_last_error(87);
    return 0;
  }
  // "S-1-<ia>-<sub>..." — authority is 6 bytes BE
  u64 ia = 0;
  for (int i = 0; i < 6; ++i) ia = (ia << 8) | s[2 + i];
  char utf8[160];
  int o = snprintf(utf8, sizeof(utf8), "S-1-%llu", (unsigned long long)ia);
  const u32 n = s[1];
  for (u32 i = 0; i < n && o < (int)sizeof(utf8) - 12; ++i) {
    const u32 sub = (u32)s[8 + i * 4] | ((u32)s[9 + i * 4] << 8) |
                    ((u32)s[10 + i * 4] << 16) | ((u32)s[11 + i * 4] << 24);
    o += snprintf(utf8 + o, sizeof(utf8) - o, "-%u", sub);
  }
  // hand the caller a UTF-16 copy in a static ring (game-pace: a handful)
  static u16 ring[4][160];
  static int ring_i = 0;
  u16* out = ring[ring_i++ % 4];
  for (int i = 0; utf8[i] && i < 159; ++i) out[i] = (u16)(u8)utf8[i];
  out[159] = 0;
  *str_out = out;
  char line[224];
  snprintf(line, sizeof(line),
           "advapi32: ConvertSidToStringSidW -> \"%s\"", utf8);
  trt_core_log(line);
  return 1;  // TRUE
}

// ---------------- ANSI Registry & QueryInfo wrappers -----------------------
namespace {
void ascii_to_u16(const char* a, u16* w, size_t cap) {
  size_t i = 0;
  if (a) {
    for (; a[i] && i + 1 < cap; ++i) w[i] = (u16)(u8)a[i];
  }
  if (cap) w[i] = 0;
}

bool bytes_look_utf16_sz(const u8* d, u32 sz) {
  return sz >= 2 && (sz % 2) == 0 && d[1] == 0;
}

struct crypt_hash_handle {
  bool used;
  trt_hash_ctx ctx;
};
crypt_hash_handle g_crypt_hashes[32];
} // namespace

int TRT_GUEST_ABI trt_RegOpenKeyExA(u64 hkey, const char* subkey, u32 options,
                                    u32 sam, void** out) {
  u16 w[256];
  if (subkey) ascii_to_u16(subkey, w, 256);
  return trt_RegOpenKeyExW(hkey, subkey ? w : nullptr, options, sam, out);
}

int TRT_GUEST_ABI trt_RegCreateKeyW(u64 hkey, const u16* subkey, void** out) {
  return trt_RegCreateKeyExW(hkey, subkey, 0, nullptr, 0, 0, out, nullptr);
}

int TRT_GUEST_ABI trt_RegOpenKeyW(u64 hkey, const u16* subkey, void** out) {
  return trt_RegOpenKeyExW(hkey, subkey, 0, 0, out);
}

int TRT_GUEST_ABI trt_RegQueryValueExA(u64 hkey, const char* name, u32* reserved,
                                       u32* type, u8* data, u32* lpcbData) {
  (void)reserved;
  load_hive();
  reg_key* key = key_for_handle(hkey);
  if (!key) return REG_ERROR_INVALID_HANDLE;
  const char* vname = (name && name[0]) ? name : "(Default)";
  reg_value* v = find_value(key, vname);
  if (!v) { char st_line[256]; snprintf(st_line, sizeof(st_line), "strtrace: RegQueryValueExA -> name=\"%.64s\" value=NULL (not found)", vname); trt_core_log(st_line); return REG_ERROR_FILE_NOT_FOUND; }
  if (type) *type = v->type;
  const bool conv_sz = (v->type == TRT_REG_SZ || v->type == TRT_REG_EXPAND_SZ) &&
                       bytes_look_utf16_sz(v->data, v->size);
  const u32 need = conv_sz ? (v->size / 2) : v->size;
  const u32 avail = lpcbData ? *lpcbData : 0;
  if (lpcbData) *lpcbData = need;
  if (!data) return REG_SUCCESS;
  if (avail < need) return REG_ERROR_MORE_DATA;
  if (conv_sz) {
    for (u32 i = 0; i < need; ++i) data[i] = v->data[i * 2];
  } else if (v->size) {
    memcpy(data, v->data, v->size);
  }
  { char st_tmp[136]; char st_line[256]; if (v->type == TRT_REG_SZ || v->type == TRT_REG_EXPAND_SZ) { size_t st_n = need; if (st_n > 132) st_n = 132; size_t st_i = 0; for (; st_i < st_n && data[st_i]; ++st_i) st_tmp[st_i] = (char)data[st_i]; st_tmp[st_i] = 0; snprintf(st_line, sizeof(st_line), "strtrace: RegQueryValueExA -> name=\"%.64s\" type=%u value=\"%.128s\"", vname, v->type, st_tmp); } else { snprintf(st_line, sizeof(st_line), "strtrace: RegQueryValueExA -> name=\"%.64s\" type=%u size=%u (non-string)", vname, v->type, v->size); } trt_core_log(st_line); }
  return REG_SUCCESS;
}

int TRT_GUEST_ABI trt_RegSetValueExA(u64 hkey, const char* name, u32 reserved,
                                     u32 type, const u8* data, u32 cbData) {
  (void)reserved;
  load_hive();
  reg_key* key = key_for_handle(hkey);
  if (!key) return REG_ERROR_INVALID_HANDLE;
  const char* vname = (name && name[0]) ? name : "(Default)";
  if ((type == TRT_REG_SZ || type == TRT_REG_EXPAND_SZ) && data && cbData > 0 && cbData <= 1024) {
    u16 wbuf[1024];
    for (u32 i = 0; i < cbData; ++i) wbuf[i] = (u16)data[i];
    set_value(key, vname, type, (const u8*)wbuf, cbData * 2);
  } else {
    set_value(key, vname, type, data, cbData);
  }
  write_hive();
  return REG_SUCCESS;
}

int TRT_GUEST_ABI trt_RegDeleteValueA(u64 hkey, const char* name) {
  u16 w[128];
  if (name) ascii_to_u16(name, w, 128);
  return trt_RegDeleteValueW(hkey, name ? w : nullptr);
}
int TRT_GUEST_ABI trt_RegDeleteKeyExA(u64 hkey, const char* subkey, u32 sam,
                                      u32 reserved) {
  u16 w[256];
  if (subkey) ascii_to_u16(subkey, w, 256);
  return trt_RegDeleteKeyExW(hkey, subkey ? w : nullptr, sam, reserved);
}
int TRT_GUEST_ABI trt_RegDeleteKeyA(u64 hkey, const char* subkey) {
  return trt_RegDeleteKeyExA(hkey, subkey, 0, 0);
}
int TRT_GUEST_ABI trt_RegDeleteTreeA(u64 hkey, const char* subkey) {
  u16 w[256];
  if (subkey) ascii_to_u16(subkey, w, 256);
  return trt_RegDeleteTreeW(hkey, subkey ? w : nullptr);
}

int TRT_GUEST_ABI trt_RegQueryInfoKeyW(u64 hkey, u16* cls, u32* cb_cls,
                                       u32* reserved, u32* subkeys,
                                       u32* max_subkey_len, u32* max_cls_len,
                                       u32* values, u32* max_val_name_len,
                                       u32* max_val_len, void* sd, void* ft) {
  (void)cls; (void)reserved; (void)sd; (void)ft;
  load_hive();
  reg_key* key = key_for_handle(hkey);
  if (!key) return REG_ERROR_INVALID_HANDLE;
  if (cb_cls) *cb_cls = 0;
  if (max_cls_len) *max_cls_len = 0;
  u32 max_sk = 0;
  for (reg_key* c = key->child; c; c = c->next_sibling) {
    const u32 l = (u32)strlen(c->name);
    if (l > max_sk) max_sk = l;
  }
  u32 max_vn = 0, max_vl = 0;
  for (reg_value* v = key->values; v; v = v->next) {
    const u32 nl = (u32)strlen(v->name);
    if (nl > max_vn) max_vn = nl;
    if (v->size > max_vl) max_vl = v->size;
  }
  if (subkeys) *subkeys = key->child_count;
  if (max_subkey_len) *max_subkey_len = max_sk;
  if (values) *values = key->value_count;
  if (max_val_name_len) *max_val_name_len = max_vn;
  if (max_val_len) *max_val_len = max_vl;
  return REG_SUCCESS;
}

// ---------------- User / HW Profile / Token Privileges ---------------------
int TRT_GUEST_ABI trt_GetUserNameW(u16* buf, u32* cch) {
  static const char k_user[] = "tiramisu";
  const u32 need = (u32)sizeof(k_user);
  if (!cch) { token_set_last_error(87); return 0; }
  if (!buf || *cch < need) {
    *cch = need;
    token_set_last_error(122);  // ERROR_INSUFFICIENT_BUFFER
    return 0;
  }
  for (u32 i = 0; i < need; ++i) buf[i] = (u16)(u8)k_user[i];
  { char st_line[256]; snprintf(st_line, sizeof(st_line), "strtrace: GetUserNameW -> buf=%p value=\"%.128s\"", (void*)buf, k_user); trt_core_log(st_line); }
  *cch = need;
  return 1;
}

int TRT_GUEST_ABI trt_GetUserNameA(char* buf, u32* cch) {
  static const char k_user[] = "tiramisu";
  const u32 need = (u32)sizeof(k_user);
  if (!cch) { token_set_last_error(87); return 0; }
  if (!buf || *cch < need) {
    *cch = need;
    token_set_last_error(122);
    return 0;
  }
  memcpy(buf, k_user, need);
  { char st_line[256]; snprintf(st_line, sizeof(st_line), "strtrace: GetUserNameA -> buf=%p value=\"%.128s\"", (void*)buf, k_user); trt_core_log(st_line); }
  *cch = need;
  return 1;
}

int TRT_GUEST_ABI trt_AdjustTokenPrivileges(void* tok, int disable_all,
                                            const void* new_state, u32 buf_len,
                                            void* prev_state, u32* ret_len) {
  (void)disable_all; (void)new_state; (void)buf_len; (void)prev_state;
  if (!token_is_valid((u32)(uintptr_t)tok)) {
    token_set_last_error(6);
    return 0;
  }
  if (ret_len) *ret_len = 0;
  token_set_last_error(0);  // ERROR_SUCCESS
  return 1;
}

int TRT_GUEST_ABI trt_LookupPrivilegeValueW(const u16* sys, const u16* name,
                                            u64* luid) {
  (void)sys; (void)name;
  if (!luid) { token_set_last_error(87); return 0; }
  *luid = 20ull;  // SE_DEBUG_PRIVILEGE nominal LUID
  return 1;
}
int TRT_GUEST_ABI trt_LookupPrivilegeValueA(const char* sys, const char* name,
                                            u64* luid) {
  (void)sys; (void)name;
  if (!luid) { token_set_last_error(87); return 0; }
  *luid = 20ull;
  return 1;
}
u32 TRT_GUEST_ABI trt_LsaNtStatusToWinError(u32 status) {
  if (status == 0) return 0;
  if (status == 0xC0000022u) return 5;    // STATUS_ACCESS_DENIED
  if (status == 0xC000000Du) return 87;   // STATUS_INVALID_PARAMETER
  if (status == 0xC0000034u) return 2;    // STATUS_OBJECT_NAME_NOT_FOUND
  if (status == 0xC0000017u) return 8;    // STATUS_NO_MEMORY
  if (status == 0x80000005u) return 234;  // STATUS_BUFFER_OVERFLOW
  return 317;                             // ERROR_MR_MID_NOT_FOUND
}
int TRT_GUEST_ABI trt_IsValidSid(const void* sid) {
  const u8* s = (const u8*)sid;
  return (s && s[0] == 1 && s[1] <= 15) ? 1 : 0;
}
u32 TRT_GUEST_ABI trt_GetLengthSid(const void* sid) {
  const u8* s = (const u8*)sid;
  if (!s) return 0;
  return 8u + (u32)s[1] * 4u;
}
int TRT_GUEST_ABI trt_EqualSid(const void* s1, const void* s2) {
  if (!trt_IsValidSid(s1) || !trt_IsValidSid(s2)) return 0;
  const u32 l1 = trt_GetLengthSid(s1);
  if (l1 != trt_GetLengthSid(s2)) return 0;
  return memcmp(s1, s2, l1) == 0 ? 1 : 0;
}
int TRT_GUEST_ABI trt_AllocateAndInitializeSid(const u8* auth, u8 sub_cnt,
                                               u32 sa0, u32 sa1, u32 sa2, u32 sa3,
                                               u32 sa4, u32 sa5, u32 sa6, u32 sa7,
                                               void** psid) {
  if (!auth || !psid || sub_cnt > 8) { token_set_last_error(87); return 0; }
  u8* s = (u8*)calloc(1, 8u + (size_t)sub_cnt * 4u);
  if (!s) { token_set_last_error(8); return 0; }
  s[0] = 1;  // SID_REVISION
  s[1] = sub_cnt;
  memcpy(s + 2, auth, 6);
  const u32 vals[8] = {sa0, sa1, sa2, sa3, sa4, sa5, sa6, sa7};
  for (u8 i = 0; i < sub_cnt; ++i) memcpy(s + 8u + i * 4u, &vals[i], 4);
  *psid = s;
  return 1;
}
void* TRT_GUEST_ABI trt_FreeSid(void* sid) {
  free(sid);
  return nullptr;
}
int TRT_GUEST_ABI trt_CheckTokenMembership(void* tok, const void* sid,
                                           int* is_member) {
  (void)tok;
  if (!is_member || !trt_IsValidSid(sid)) { token_set_last_error(87); return 0; }
  const u8* s = (const u8*)sid;
  u32 rid0 = 0, rid1 = 0;
  if (s[1] >= 1) memcpy(&rid0, s + 8, 4);
  if (s[1] >= 2) memcpy(&rid1, s + 12, 4);
  // Not elevated Administrators (S-1-5-32-544), is a normal Users member (S-1-5-32-545)
  *is_member = (rid0 == 32u && rid1 == 545u) ? 1 : 0;
  return 1;
}

u32* TRT_GUEST_ABI trt_GetSidSubAuthority(void* sid, u32 sub_auth) {
  u8* s = (u8*)sid;
  if (!s || sub_auth >= s[1]) { token_set_last_error(87); return nullptr; }
  return (u32*)(s + 8 + sub_auth * 4);
}

u8* TRT_GUEST_ABI trt_GetSidSubAuthorityCount(void* sid) {
  u8* s = (u8*)sid;
  if (!s) { token_set_last_error(87); return nullptr; }
  return s + 1;
}

int TRT_GUEST_ABI trt_GetCurrentHwProfileA(void* info) {
  if (!info) { token_set_last_error(87); return 0; }
  // HW_PROFILE_INFOA: dwDockInfo (u32), szHwProfileGuid[39], szHwProfileName[80]
  memset(info, 0, 124);
  *(u32*)info = 0x1;  // DOCKINFO_UNDOCKED
  memcpy((u8*)info + 4, "{tiramisu-0000-0000-0000-xboxseries}", 37);
  memcpy((u8*)info + 43, "TiramisuXbox", 13);
  return 1;
}

// ---------------- CryptoAPI (advapi32) backed by bridge_crypt32 ----------
int TRT_GUEST_ABI trt_CryptAcquireContextW(void** phProv, const u16* container,
                                           const u16* provider, u32 prov_type,
                                           u32 flags) {
  (void)container; (void)provider; (void)prov_type; (void)flags;
  if (!phProv) { token_set_last_error(87); return 0; }
  *phProv = (void*)(uintptr_t)0xC4190001u;
  return 1;
}

int TRT_GUEST_ABI trt_CryptAcquireContextA(void** phProv, const char* container,
                                           const char* provider, u32 prov_type,
                                           u32 flags) {
  (void)container; (void)provider; (void)prov_type; (void)flags;
  if (!phProv) { token_set_last_error(87); return 0; }
  *phProv = (void*)(uintptr_t)0xC4190001u;
  return 1;
}

int TRT_GUEST_ABI trt_CryptReleaseContext(void* hProv, u32 flags) {
  (void)hProv; (void)flags;
  return 1;
}

int TRT_GUEST_ABI trt_CryptGenRandom(void* hProv, u32 len, u8* buf) {
  (void)hProv;
  if (!buf && len > 0) { token_set_last_error(87); return 0; }
  trt_csprng_fill(buf, len);
  return 1;
}

int TRT_GUEST_ABI trt_CryptCreateHash(void* hProv, u32 alg_id, void* hKey,
                                      u32 flags, void** phHash) {
  (void)hProv; (void)hKey; (void)flags;
  if (!phHash) { token_set_last_error(87); return 0; }
  u32 ha = TRT_HASH_NONE;
  if (alg_id == 0x8003u) ha = TRT_HASH_MD5;          // CALG_MD5
  else if (alg_id == 0x8004u) ha = TRT_HASH_SHA1;    // CALG_SHA1
  else if (alg_id == 0x800Cu) ha = TRT_HASH_SHA256;  // CALG_SHA_256
  if (!ha) { token_set_last_error(0x80090008u); return 0; }  // NTE_BAD_ALGID
  for (u32 i = 0; i < 32; ++i) {
    if (!g_crypt_hashes[i].used) {
      g_crypt_hashes[i].used = true;
      trt_hash_init(&g_crypt_hashes[i].ctx, ha);
      *phHash = (void*)(uintptr_t)(0xC41A0000u | i);
      return 1;
    }
  }
  token_set_last_error(8);
  return 0;
}

int TRT_GUEST_ABI trt_CryptHashData(void* hHash, const u8* data, u32 len,
                                    u32 flags) {
  (void)flags;
  const u32 h = (u32)(uintptr_t)hHash;
  if ((h >> 16) != 0xC41Au || (h & 0xFFFFu) >= 32 ||
      !g_crypt_hashes[h & 0xFFFFu].used) {
    token_set_last_error(6);
    return 0;
  }
  trt_hash_update(&g_crypt_hashes[h & 0xFFFFu].ctx, data, len);
  return 1;
}

int TRT_GUEST_ABI trt_CryptGetHashParam(void* hHash, u32 param, u8* data,
                                        u32* data_len, u32 flags) {
  (void)flags;
  const u32 h = (u32)(uintptr_t)hHash;
  if ((h >> 16) != 0xC41Au || (h & 0xFFFFu) >= 32 ||
      !g_crypt_hashes[h & 0xFFFFu].used || !data_len) {
    token_set_last_error(6);
    return 0;
  }
  trt_hash_ctx copy = g_crypt_hashes[h & 0xFFFFu].ctx;
  const u32 dlen = trt_hash_digest_len(copy.alg);
  if (param == 4 /* HP_HASHSIZE */) {
    if (data && *data_len >= 4) { *(u32*)data = dlen; *data_len = 4; return 1; }
    *data_len = 4;
    if (!data) return 1;
    token_set_last_error(234);
    return 0;
  }
  if (param == 2 /* HP_HASHVAL */) {
    if (!data) { *data_len = dlen; return 1; }
    if (*data_len < dlen) { *data_len = dlen; token_set_last_error(234); return 0; }
    trt_hash_final(&copy, data);
    *data_len = dlen;
    return 1;
  }
  token_set_last_error(87);
  return 0;
}

int TRT_GUEST_ABI trt_CryptDestroyHash(void* hHash) {
  const u32 h = (u32)(uintptr_t)hHash;
  if ((h >> 16) != 0xC41Au || (h & 0xFFFFu) >= 32) {
    token_set_last_error(6);
    return 0;
  }
  g_crypt_hashes[h & 0xFFFFu].used = false;
  return 1;
}

// ETW Event* no-ops directly exported on advapi32 (besides ntdll forward)
u32 TRT_GUEST_ABI reg_EventRegister(const void* id, void* cb, void* ctx, u64* h) {
  (void)id; (void)cb; (void)ctx;
  if (h) *h = 0x45545701ull;
  return 0;
}
u32 TRT_GUEST_ABI reg_EventUnregister(u64) { return 0; }
u32 TRT_GUEST_ABI reg_EventWrite(u64, const void*, u32, void*) { return 0; }
u32 TRT_GUEST_ABI reg_EventWriteEx(u64, const void*, u64, u32, const void*,
                                   const void*, u32, void*) { return 0; }
u32 TRT_GUEST_ABI reg_EventWriteTransfer(u64, const void*, const void*,
                                         const void*, u32, void*) { return 0; }
u32 TRT_GUEST_ABI reg_EventWriteString(u64, u8, u64, const u16*) { return 0; }
u32 TRT_GUEST_ABI reg_EventSetInformation(u64, u32, const void*, u32) { return 0; }
u32 TRT_GUEST_ABI reg_EventActivityIdControl(u32 ctrl, void* guid) {
  if (!guid) return 87;
  if (ctrl == 1 || ctrl == 3 || ctrl == 4) memset(guid, 0, 16);
  return 0;
}

// advapi32!SystemFunction036 (RtlGenRandom): Win64 guest-ABI CSPRNG fill.
static int TRT_GUEST_ABI trt_SystemFunction036(void* buf, unsigned long len) {
  trt_csprng_fill((u8*)buf, (u32)len);
  return 1;  // TRUE
}

// exported for the test harness (the dstoragecore IAT-gap bind)
void* trt_bridge_SystemFunction036(void* buf, unsigned long len) {
  return (void*)(uintptr_t)trt_SystemFunction036(buf, len);
}

u32 TRT_GUEST_ABI reg_EnumDynamicTimeZoneInformation(u32 index, void* info) {
  if (!info) return 87;  // ERROR_INVALID_PARAMETER
  if (index > 0) return 259;  // ERROR_NO_MORE_ITEMS
  u8* b = (u8*)info;
  memset(b, 0, 432);
  static const u16 k_utc[] = {'U', 'T', 'C', 0};
  memcpy(b + 4, k_utc, sizeof(k_utc));     // StandardName
  memcpy(b + 88, k_utc, sizeof(k_utc));    // DaylightName
  memcpy(b + 172, k_utc, sizeof(k_utc));   // TimeZoneKeyName
  return 0;  // ERROR_SUCCESS
}

u32 TRT_GUEST_ABI reg_GetDynamicTimeZoneInformationEffectiveYears(
    const void* info, u32* first_year, u32* last_year) {
  (void)info;
  if (!first_year || !last_year) return 87;
  *first_year = 2020;
  *last_year = 2030;
  return 0;
}

u32 TRT_GUEST_ABI reg_EnumerateTraceGuidsEx(u32 info_class, const void* in_buf,
                                            u32 in_sz, void* out_buf,
                                            u32 out_sz, u32* req_sz) {
  (void)info_class; (void)in_buf; (void)in_sz; (void)out_buf; (void)out_sz;
  if (req_sz) *req_sz = 0;
  return 0;  // ERROR_SUCCESS (zero active ETW trace sessions)
}

// ---- Sekiro audit gaps (2026-11-21) ----
// sekiro.exe and fmodex64.dll import 15 more advapi32 functions than the
// registry/token/CryptoAPI core above. RegOpenKeyA and RegEnumKeyA are real
// wrappers onto the ExW implementations; the rest are fail-visible: no
// crypto-provider keys, no event log, and no security model exist on the
// console, so they log the call and return the API's own failure value
// instead of killing the load.
int TRT_GUEST_ABI trt_RegOpenKeyA(u64 hkey, const char* subkey, void** out) {
  return trt_RegOpenKeyExA(hkey, subkey, 0, 0, out);
}

int TRT_GUEST_ABI trt_RegEnumKeyA(u64 hkey, u32 index, char* name,
                                  u32 cchName) {
  u16 w[256];
  u32 len = 0;
  const int rc =
      trt_RegEnumKeyExW(hkey, index, w, 256, nullptr, &len, nullptr);
  if (rc != REG_SUCCESS) return rc;
  if (!name) return REG_SUCCESS;
  if (cchName < len) return REG_ERROR_MORE_DATA;
  for (u32 i = 0; i < len; ++i) name[i] = (char)(w[i] & 0x7f);
  return REG_SUCCESS;
}

int TRT_GUEST_ABI trt_CryptDecrypt(u64 hKey, u64 hHash, int final, u32 flags,
                                   u8* data, u32* data_len) {
  (void)hKey; (void)hHash; (void)final; (void)flags; (void)data;
  (void)data_len;
  trt_core_log("advapi32: CryptDecrypt -> FALSE (no crypto keys on console)");
  return 0;
}

int TRT_GUEST_ABI trt_CryptDestroyKey(u64 hKey) {
  (void)hKey;
  trt_core_log("advapi32: CryptDestroyKey -> FALSE (no crypto keys)");
  return 0;
}

int TRT_GUEST_ABI trt_CryptEnumProvidersW(u32 index, u32* reserved, u32 flags,
                                          u32* prov_type, u16* prov_name,
                                          u32* prov_name_len) {
  (void)index; (void)reserved; (void)flags; (void)prov_type;
  (void)prov_name; (void)prov_name_len;
  trt_core_log("advapi32: CryptEnumProvidersW -> FALSE (no providers)");
  return 0;
}

int TRT_GUEST_ABI trt_CryptExportKey(u64 hKey, u64 hExpKey, u32 blob_type,
                                     u32 flags, u8* data, u32* data_len) {
  (void)hKey; (void)hExpKey; (void)blob_type; (void)flags; (void)data;
  (void)data_len;
  trt_core_log("advapi32: CryptExportKey -> FALSE (no crypto keys)");
  return 0;
}

int TRT_GUEST_ABI trt_CryptGetProvParam(u64 hProv, u32 param, u8* data,
                                        u32* data_len, u32 flags) {
  (void)hProv; (void)param; (void)data; (void)data_len; (void)flags;
  trt_core_log("advapi32: CryptGetProvParam -> FALSE (no provider params)");
  return 0;
}

int TRT_GUEST_ABI trt_CryptGetUserKey(u64 hProv, u32 key_spec, u64* user_key) {
  (void)hProv; (void)key_spec; (void)user_key;
  trt_core_log("advapi32: CryptGetUserKey -> FALSE (no user keys)");
  return 0;
}

int TRT_GUEST_ABI trt_CryptSetHashParam(u64 hHash, u32 param, const u8* data,
                                        u32 flags) {
  (void)hHash; (void)param; (void)data; (void)flags;
  trt_core_log("advapi32: CryptSetHashParam -> FALSE (hash params unsupported)");
  return 0;
}

int TRT_GUEST_ABI trt_CryptSignHashW(u64 hHash, u32 key_spec,
                                     const u16* description, u32 flags,
                                     u8* signature, u32* sig_len) {
  (void)hHash; (void)key_spec; (void)description; (void)flags;
  (void)signature; (void)sig_len;
  trt_core_log("advapi32: CryptSignHashW -> FALSE (no signing keys)");
  return 0;
}

void* TRT_GUEST_ABI trt_RegisterEventSourceW(const u16* server,
                                             const u16* source) {
  (void)server; (void)source;
  trt_core_log("advapi32: RegisterEventSourceW -> NULL (no event log)");
  return nullptr;
}

int TRT_GUEST_ABI trt_DeregisterEventSource(void* hEventLog) {
  (void)hEventLog;
  return 1;  // nothing was ever registered
}

int TRT_GUEST_ABI trt_ReportEventW(void* hEventLog, u16 type, u16 category,
                                   u32 event_id, void* user_sid,
                                   u16 num_strings, u32 data_size,
                                   const u16** strings, void* data) {
  (void)hEventLog; (void)type; (void)category; (void)event_id;
  (void)user_sid; (void)num_strings; (void)data_size; (void)strings;
  (void)data;
  trt_core_log("advapi32: ReportEventW -> FALSE (no event log)");
  return 0;
}

int TRT_GUEST_ABI trt_InitializeSecurityDescriptor(void* sd, u32 revision) {
  (void)sd; (void)revision;
  trt_core_log("advapi32: InitializeSecurityDescriptor -> FALSE (no security model)");
  return 0;
}

int TRT_GUEST_ABI trt_SetSecurityDescriptorDacl(void* sd, int dacl_present,
                                                void* acl, int dacl_defaulted) {
  (void)sd; (void)dacl_present; (void)acl; (void)dacl_defaulted;
  trt_core_log("advapi32: SetSecurityDescriptorDacl -> FALSE (no security model)");
  return 0;
}

int trt_bridge_register_registry(void) {
  static const trt_named_fn k_advapi32_fns[] = {
      {"RegOpenKeyExW", (void*)&trt_RegOpenKeyExW},
      {"RegOpenKeyExA", (void*)&trt_RegOpenKeyExA},
      {"RegCreateKeyExW", (void*)&trt_RegCreateKeyExW},
      {"RegCreateKeyW", (void*)&trt_RegCreateKeyW},
      {"RegOpenKeyW", (void*)&trt_RegOpenKeyW},
      {"RegOpenKeyA", (void*)&trt_RegOpenKeyA},
      {"RegQueryValueExW", (void*)&trt_RegQueryValueExW},
      {"RegQueryValueExA", (void*)&trt_RegQueryValueExA},
      {"RegSetValueExW", (void*)&trt_RegSetValueExW},
      {"RegSetValueExA", (void*)&trt_RegSetValueExA},
      {"RegDeleteValueW", (void*)&trt_RegDeleteValueW},
      {"RegDeleteValueA", (void*)&trt_RegDeleteValueA},
      {"RegQueryInfoKeyW", (void*)&trt_RegQueryInfoKeyW},
      {"RegCloseKey", (void*)&trt_RegCloseKey},
      {"RegEnumKeyExW", (void*)&trt_RegEnumKeyExW},
      {"RegEnumKeyA", (void*)&trt_RegEnumKeyA},
      {"RegEnumValueW", (void*)&trt_RegEnumValueW},
      {"RegGetValueW", (void*)&trt_RegGetValueW},
      {"GetUserNameW", (void*)&trt_GetUserNameW},
      {"GetUserNameA", (void*)&trt_GetUserNameA},
      {"GetCurrentHwProfileA", (void*)&trt_GetCurrentHwProfileA},
      // the token/security half (the honest no-security-model surface)
      {"OpenProcessToken", (void*)&trt_OpenProcessToken},
      {"GetTokenInformation", (void*)&trt_GetTokenInformation},
      {"AdjustTokenPrivileges", (void*)&trt_AdjustTokenPrivileges},
      {"LookupPrivilegeValueW", (void*)&trt_LookupPrivilegeValueW},
      {"GetSidSubAuthority", (void*)&trt_GetSidSubAuthority},
      {"GetSidSubAuthorityCount", (void*)&trt_GetSidSubAuthorityCount},
      {"ConvertStringSecurityDescriptorToSecurityDescriptorW",
       (void*)&trt_ConvertStringSecurityDescriptorToSecurityDescriptorW},
      {"ConvertSidToStringSidW", (void*)&trt_ConvertSidToStringSidW},
      // Event log + security descriptor (Sekiro audit: no event log and
      // no security model on the console -- fail-visible stubs above)
      {"RegisterEventSourceW", (void*)&trt_RegisterEventSourceW},
      {"DeregisterEventSource", (void*)&trt_DeregisterEventSource},
      {"ReportEventW", (void*)&trt_ReportEventW},
      {"InitializeSecurityDescriptor", (void*)&trt_InitializeSecurityDescriptor},
      {"SetSecurityDescriptorDacl", (void*)&trt_SetSecurityDescriptorDacl},
      // CryptoAPI + RtlGenRandom
      {"CryptAcquireContextW", (void*)&trt_CryptAcquireContextW},
      {"CryptAcquireContextA", (void*)&trt_CryptAcquireContextA},
      {"CryptReleaseContext", (void*)&trt_CryptReleaseContext},
      {"CryptGenRandom", (void*)&trt_CryptGenRandom},
      {"CryptCreateHash", (void*)&trt_CryptCreateHash},
      {"CryptHashData", (void*)&trt_CryptHashData},
      {"CryptGetHashParam", (void*)&trt_CryptGetHashParam},
      {"CryptDestroyHash", (void*)&trt_CryptDestroyHash},
      {"CryptDecrypt", (void*)&trt_CryptDecrypt},
      {"CryptDestroyKey", (void*)&trt_CryptDestroyKey},
      {"CryptEnumProvidersW", (void*)&trt_CryptEnumProvidersW},
      {"CryptExportKey", (void*)&trt_CryptExportKey},
      {"CryptGetProvParam", (void*)&trt_CryptGetProvParam},
      {"CryptGetUserKey", (void*)&trt_CryptGetUserKey},
      {"CryptSetHashParam", (void*)&trt_CryptSetHashParam},
      {"CryptSignHashW", (void*)&trt_CryptSignHashW},
      {"SystemFunction036", (void*)&trt_SystemFunction036},
      // ETW & Dynamic TimeZone
      {"EventRegister", (void*)&reg_EventRegister},
      {"EventUnregister", (void*)&reg_EventUnregister},
      {"EventWrite", (void*)&reg_EventWrite},
      {"EventWriteEx", (void*)&reg_EventWriteEx},
      {"EventWriteTransfer", (void*)&reg_EventWriteTransfer},
      {"EventWriteString", (void*)&reg_EventWriteString},
      {"EventSetInformation", (void*)&reg_EventSetInformation},
      {"EventActivityIdControl", (void*)&reg_EventActivityIdControl},
      {"EnumDynamicTimeZoneInformation",
       (void*)&reg_EnumDynamicTimeZoneInformation},
      {"GetDynamicTimeZoneInformationEffectiveYears",
       (void*)&reg_GetDynamicTimeZoneInformationEffectiveYears},
      {"EnumerateTraceGuidsEx", (void*)&reg_EnumerateTraceGuidsEx},
      {"RegDeleteKeyExW", (void*)&trt_RegDeleteKeyExW},
      {"RegDeleteKeyExA", (void*)&trt_RegDeleteKeyExA},
      {"RegDeleteKeyW", (void*)&trt_RegDeleteKeyW},
      {"RegDeleteKeyA", (void*)&trt_RegDeleteKeyA},
      {"RegDeleteTreeW", (void*)&trt_RegDeleteTreeW},
      {"RegDeleteTreeA", (void*)&trt_RegDeleteTreeA},
      {"RegFlushKey", (void*)&trt_RegFlushKey},
      {"RegOpenCurrentUser", (void*)&trt_RegOpenCurrentUser},
      {"RegConnectRegistryW", (void*)&trt_RegConnectRegistryW},
      {"RegSaveKeyW", (void*)&trt_RegSaveKeyW},
      {"RegLoadKeyW", (void*)&trt_RegLoadKeyW},
      {"LookupPrivilegeValueA", (void*)&trt_LookupPrivilegeValueA},
      {"LsaNtStatusToWinError", (void*)&trt_LsaNtStatusToWinError},
      {"IsValidSid", (void*)&trt_IsValidSid},
      {"GetLengthSid", (void*)&trt_GetLengthSid},
      {"EqualSid", (void*)&trt_EqualSid},
      {"AllocateAndInitializeSid", (void*)&trt_AllocateAndInitializeSid},
      {"FreeSid", (void*)&trt_FreeSid},
      {"CheckTokenMembership", (void*)&trt_CheckTokenMembership},
  };
  const int n_adv = (int)(sizeof(k_advapi32_fns) / sizeof(k_advapi32_fns[0]));
  trt_module_register("advapi32.dll", TRT_ROUTE_BUILTIN, k_advapi32_fns, n_adv);
  trt_module_register("api-ms-win-core-registry-l1-1-0.dll", TRT_ROUTE_BUILTIN,
                      k_advapi32_fns, n_adv);
  trt_module_register("api-ms-win-core-registry-l1-1-1.dll", TRT_ROUTE_BUILTIN,
                      k_advapi32_fns, n_adv);
  trt_module_register("api-ms-win-core-registry-l2-1-0.dll", TRT_ROUTE_BUILTIN,
                      k_advapi32_fns, n_adv);
  return 0;
}

} // extern "C"
