// tiramisu/src/trt/vfs.cpp
// Virtual filesystem: the guest sees a Windows world; Tiramisu translates.
//   C:\ -> <writable>/drive_c      (game installs, saves, registry file)
//   Z:\ -> package install dir     (read-only: the game's own files)
//   D:\ -> a disc image (.iso/.bin/.img), read-only, lazily extracted
// Rules are data, so per-game overrides ship as JSON later, not code.
#include "trt.h"
#include "iso.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <direct.h>
#  include <io.h>
#  include <sys/stat.h>
static int vfs_mkdir_one(const char* p) { return _mkdir(p); }
static int vfs_rmdir_one(const char* p) { return _rmdir(p); }
#else
#  include <dirent.h>
#  include <sys/stat.h>
#  include <unistd.h>
static int vfs_mkdir_one(const char* p) { return mkdir(p, 0755); }
static int vfs_rmdir_one(const char* p) { return rmdir(p); }
#endif

namespace {

char lower_of(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }
char upper_of(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c; }

struct vfs_mount {
  char prefix[8];   // lower-case ASCII drive prefix, e.g. "c:"
  char target[512]; // host path (UTF-8)
};
vfs_mount g_mounts[8];
int g_mount_count = 0;

// Disc images mounted as read-only drives. A disc game's files are read out of
// the image on demand and cached on the writable drive; the image itself is
// never copied wholesale and never written.
struct vfs_image {
  char prefix[8];
  char image_path[512];
  trt_iso* iso;
};
vfs_image g_images[8];
int g_image_count = 0;
char g_iso_cache_root[512] = "";

// guest current-working-directory drive and full normalized directory
char g_cwd_drive[8] = "z:";
char g_cwd_guest[512] = "";

volatile long g_vfs_lock_word = 0;
void vfs_lock(void) {
  while (__atomic_exchange_n(&g_vfs_lock_word, 1, __ATOMIC_ACQUIRE) != 0) {}
}
void vfs_unlock(void) {
  __atomic_store_n(&g_vfs_lock_word, 0, __ATOMIC_RELEASE);
}

struct str_vec {
  char** items;
  size_t count;
  size_t cap;
};
str_vec g_z_deleted = {};
str_vec g_z_opaque = {};
char g_whiteout_c_root[512] = "";

char* dup_str(const char* s) {
  size_t n = strlen(s);
  char* p = (char*)malloc(n + 1);
  if (p) memcpy(p, s, n + 1);
  return p;
}

void vec_clear(str_vec* v) {
  for (size_t i = 0; i < v->count; ++i) free(v->items[i]);
  free(v->items);
  v->items = nullptr;
  v->count = 0;
  v->cap = 0;
}

bool vec_has(const str_vec* v, const char* s) {
  for (size_t i = 0; i < v->count; ++i)
    if (strcmp(v->items[i], s) == 0) return true;
  return false;
}

void vec_add(str_vec* v, const char* s) {
  if (!s || !s[0] || vec_has(v, s)) return;
  if (v->count == v->cap) {
    size_t nc = v->cap ? v->cap * 2 : 16;
    char** ni = (char**)realloc(v->items, nc * sizeof(char*));
    if (!ni) return;
    v->items = ni;
    v->cap = nc;
  }
  char* cp = dup_str(s);
  if (cp) v->items[v->count++] = cp;
}

void vec_remove_exact(str_vec* v, const char* s) {
  for (size_t i = 0; i < v->count; ++i) {
    if (strcmp(v->items[i], s) == 0) {
      free(v->items[i]);
      v->items[i] = v->items[--v->count];
      return;
    }
  }
}

void vec_remove_tree(str_vec* v, const char* prefix) {
  size_t plen = strlen(prefix);
  for (size_t i = 0; i < v->count; ) {
    const char* s = v->items[i];
    if (strcmp(s, prefix) == 0 ||
        (strncmp(s, prefix, plen) == 0 && s[plen] == '/')) {
      free(v->items[i]);
      v->items[i] = v->items[--v->count];
    } else {
      ++i;
    }
  }
}

void to_lower_rel(const char* in, char* out, size_t cap) {
  if (!cap) return;
  size_t i = 0;
  for (; in && in[i] && i + 1 < cap; ++i) {
    char c = in[i];
    out[i] = (c == '\\') ? '/' : lower_of(c);
  }
  while (i > 0 && out[i - 1] == '/') --i;
  out[i] = 0;
}

void ensure_whiteouts_loaded_locked(const char* c_target) {
  if (!c_target || !c_target[0]) return;
  if (strcmp(g_whiteout_c_root, c_target) == 0) return;
  vec_clear(&g_z_deleted);
  vec_clear(&g_z_opaque);
  snprintf(g_whiteout_c_root, sizeof(g_whiteout_c_root), "%s", c_target);
  char idx_path[1024];
  snprintf(idx_path, sizeof(idx_path), "%s/_z_whiteouts.idx", c_target);
  FILE* f = fopen(idx_path, "rb");
  if (!f) return;
  char line[1100];
  while (fgets(line, sizeof(line), f)) {
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
      line[--len] = 0;
    if (len > 2 && (line[0] == 'D' || line[0] == 'O') && line[1] == ' ') {
      char rel[1024];
      to_lower_rel(line + 2, rel, sizeof(rel));
      if (rel[0]) vec_add(line[0] == 'D' ? &g_z_deleted : &g_z_opaque, rel);
    }
  }
  fclose(f);
}

void persist_whiteouts_locked(const char* c_target) {
  if (!c_target || !c_target[0]) return;
  char idx_path[1024];
  snprintf(idx_path, sizeof(idx_path), "%s/_z_whiteouts.idx", c_target);
  if (g_z_deleted.count == 0 && g_z_opaque.count == 0) {
    remove(idx_path);
    return;
  }
  FILE* f = fopen(idx_path, "wb");
  if (!f) return;
  for (size_t i = 0; i < g_z_deleted.count; ++i)
    fprintf(f, "D %s\n", g_z_deleted.items[i]);
  for (size_t i = 0; i < g_z_opaque.count; ++i)
    fprintf(f, "O %s\n", g_z_opaque.items[i]);
  fclose(f);
}

bool is_rel_whited_out_locked(const char* rel_lower, bool ov_exists) {
  if (!rel_lower || !rel_lower[0]) return false;
  if (vec_has(&g_z_deleted, rel_lower)) return true;
  char anc[1024];
  snprintf(anc, sizeof(anc), "%s", rel_lower);
  for (;;) {
    char* slash = strrchr(anc, '/');
    if (!slash) break;
    *slash = 0;
    if (!anc[0]) break;
    if (vec_has(&g_z_deleted, anc)) return true;
    if (vec_has(&g_z_opaque, anc) && !ov_exists) return true;
  }
  return false;
}

bool is_dir_opaque_locked(const char* rel_lower) {
  if (!rel_lower || !rel_lower[0]) return false;
  if (vec_has(&g_z_opaque, rel_lower)) return true;
  char anc[1024];
  snprintf(anc, sizeof(anc), "%s", rel_lower);
  for (;;) {
    char* slash = strrchr(anc, '/');
    if (!slash) break;
    *slash = 0;
    if (!anc[0]) break;
    if (vec_has(&g_z_opaque, anc)) return true;
  }
  return false;
}

void effective_cwd_locked(char drv_out[3], char* rel_out, size_t rel_cap) {
  char d = lower_of(g_cwd_drive[0]);
  if (d < 'a' || d > 'z') d = 'z';
  drv_out[0] = d;
  drv_out[1] = ':';
  drv_out[2] = 0;
  rel_out[0] = 0;
  if (g_cwd_guest[0] && lower_of(g_cwd_guest[0]) == d && g_cwd_guest[1] == ':') {
    const char* s = g_cwd_guest + 2;
    while (*s == '\\' || *s == '/') ++s;
    size_t i = 0;
    for (; s[i] && i + 1 < rel_cap; ++i)
      rel_out[i] = (s[i] == '\\') ? '/' : s[i];
    while (i > 0 && rel_out[i - 1] == '/') --i;
    rel_out[i] = 0;
  }
}

int normalize_guest_locked(const char* utf8_in, char drv_out[3],
                           char* rel_out, size_t rel_cap,
                           char* guest_out, size_t guest_cap,
                           bool preserve_trailing_slash) {
  if (!utf8_in || !utf8_in[0] || !rel_out || rel_cap == 0) return -1;
  const char* in = utf8_in;
  if ((in[0] == '\\' || in[0] == '/') && (in[1] == '\\' || in[1] == '/') &&
      (in[2] == '?' || in[2] == '.') && (in[3] == '\\' || in[3] == '/') &&
      lower_of(in[4]) >= 'a' && lower_of(in[4]) <= 'z' && in[5] == ':') {
    in += 4;
  } else if (in[0] == '\\' && in[1] == '?' && in[2] == '?' && in[3] == '\\' &&
             lower_of(in[4]) >= 'a' && lower_of(in[4]) <= 'z' && in[5] == ':') {
    in += 4;
  }

  size_t in_len = strlen(in);
  bool had_trail = preserve_trailing_slash && in_len > 0 &&
                   (in[in_len - 1] == '\\' || in[in_len - 1] == '/');

  char cur_drv[3], cur_rel[512];
  effective_cwd_locked(cur_drv, cur_rel, sizeof(cur_rel));

  const char* p = in;
  const char* seed = nullptr;
  if (lower_of(p[0]) == 'g' && lower_of(p[1]) == 'a' && lower_of(p[2]) == 'm' &&
      lower_of(p[3]) == 'e' && p[4] == ':') {
    drv_out[0] = 'z';
    drv_out[1] = ':';
    drv_out[2] = 0;
    p += 5;
    if (*p == '\\' || *p == '/') {
      seed = nullptr;
      while (*p == '\\' || *p == '/') ++p;
    } else {
      seed = cur_rel;
    }
  } else if (lower_of(p[0]) == 'm' && lower_of(p[1]) == 's' && p[2] == '-' &&
             lower_of(p[3]) == 'a' && lower_of(p[4]) == 'p' && lower_of(p[5]) == 'p' &&
             lower_of(p[6]) == 'x' && p[7] == ':') {
    drv_out[0] = 'z';
    drv_out[1] = ':';
    drv_out[2] = 0;
    p += 8;
    if (*p == '\\' || *p == '/') {
      seed = nullptr;
      while (*p == '\\' || *p == '/') ++p;
    } else {
      seed = cur_rel;
    }
  } else if (lower_of(p[0]) >= 'a' && lower_of(p[0]) <= 'z' && p[1] == ':') {
    drv_out[0] = lower_of(p[0]);
    drv_out[1] = ':';
    drv_out[2] = 0;
    p += 2;
    if (*p == '\\' || *p == '/') seed = nullptr;
    else if (drv_out[0] == cur_drv[0]) seed = cur_rel;
    else seed = nullptr;
  } else if (p[0] == '\\' || p[0] == '/') {
    drv_out[0] = cur_drv[0];
    drv_out[1] = ':';
    drv_out[2] = 0;
    seed = nullptr;
  } else {
    drv_out[0] = cur_drv[0];
    drv_out[1] = ':';
    drv_out[2] = 0;
    seed = cur_rel;
  }

  const char* seg_p[64];
  size_t seg_l[64];
  int depth = 0;
  auto feed_tokens = [&](const char* src) {
    if (!src) return;
    while (*src) {
      while (*src == '\\' || *src == '/') ++src;
      if (!*src) break;
      const char* start = src;
      while (*src && *src != '\\' && *src != '/') ++src;
      size_t len = (size_t)(src - start);
      if (len == 1 && start[0] == '.') continue;
      if (len == 2 && start[0] == '.' && start[1] == '.') {
        if (depth > 0) --depth;
        continue;
      }
      if (depth < 64) {
        seg_p[depth] = start;
        seg_l[depth] = len;
        ++depth;
      }
    }
  };
  feed_tokens(seed);
  feed_tokens(p);

  size_t rpos = 0;
  for (int i = 0; i < depth; ++i) {
    if (i > 0 && rpos + 1 < rel_cap) rel_out[rpos++] = '/';
    size_t copy = seg_l[i];
    if (rpos + copy >= rel_cap) copy = rel_cap > rpos ? rel_cap - rpos - 1 : 0;
    if (copy > 0) { memcpy(rel_out + rpos, seg_p[i], copy); rpos += copy; }
  }
  rel_out[rpos] = 0;

  if (guest_out && guest_cap >= 4) {
    size_t gpos = 0;
    guest_out[gpos++] = upper_of(drv_out[0]);
    guest_out[gpos++] = ':';
    guest_out[gpos++] = '\\';
    for (int i = 0; i < depth; ++i) {
      if (i > 0 && gpos + 1 < guest_cap) guest_out[gpos++] = '\\';
      size_t copy = seg_l[i];
      if (gpos + copy >= guest_cap) copy = guest_cap > gpos ? guest_cap - gpos - 1 : 0;
      if (copy > 0) { memcpy(guest_out + gpos, seg_p[i], copy); gpos += copy; }
    }
    if (depth > 0 && had_trail && gpos + 1 < guest_cap) guest_out[gpos++] = '\\';
    guest_out[gpos] = 0;
  }
  return 0;
}

void utf16_to_utf8(const u16* in, char* out, size_t out_cap) {
  size_t o = 0;
  for (size_t i = 0; in[i] && o + 4 <= out_cap; ++i) {
    u32 c = in[i];
    if (c >= 0xD800u && c < 0xDC00u && in[i + 1] >= 0xDC00u && in[i + 1] < 0xE000u) {
      c = 0x10000u + ((c - 0xD800u) << 10) + (in[i + 1] - 0xDC00u);
      ++i;
    }
    if (c < 0x80) out[o++] = (char)c;
    else if (c < 0x800) {
      out[o++] = (char)(0xC0 | (c >> 6));
      out[o++] = (char)(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      out[o++] = (char)(0xE0 | (c >> 12));
      out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
      out[o++] = (char)(0x80 | (c & 0x3F));
    } else {
      out[o++] = (char)(0xF0 | (c >> 18));
      out[o++] = (char)(0x80 | ((c >> 12) & 0x3F));
      out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
      out[o++] = (char)(0x80 | (c & 0x3F));
    }
  }
  out[o] = 0;
}

} // namespace

extern "C" {

// UTF-16 -> UTF-8 (shared with the loader bridge: LoadLibraryW paths)
int trt_utf16_to_utf8(const u16* in, char* out, size_t out_cap) {
  if (!in || !out || out_cap == 0) return -1;
  utf16_to_utf8(in, out, out_cap);
  return 0;
}

void trt_vfs_mount(const char* drive_prefix, const char* host_path) {
  if (!drive_prefix || !host_path) return;
  char norm_p[8] = {};
  for (size_t i = 0; i < 7 && drive_prefix[i]; ++i)
    norm_p[i] = lower_of(drive_prefix[i]);
  int slot = -1;
  for (int i = 0; i < g_mount_count; ++i) {
    if (strcmp(g_mounts[i].prefix, norm_p) == 0) { slot = i; break; }
  }
  if (slot < 0) {
    if (g_mount_count >= 8) return;
    slot = g_mount_count++;
  }
  vfs_mount& m = g_mounts[slot];
  memset(&m, 0, sizeof(m));
  strncpy(m.prefix, norm_p, sizeof(m.prefix) - 1);
  strncpy(m.target, host_path, sizeof(m.target) - 1);
  // the guest cwd drive: Z: when mounted (exe's dir), else the first mount
  if (strcmp(m.prefix, "z:") == 0 || g_mount_count == 1)
    strncpy(g_cwd_drive, m.prefix, sizeof(g_cwd_drive) - 1);
}

int trt_vfs_stat(const char* host_path, int* is_dir, u64* size_out) {
  if (!host_path || !*host_path) return -1;
  char tmp[1024];
  snprintf(tmp, sizeof(tmp), "%s", host_path);
  for (char* q = tmp; *q; ++q)
    if (*q == '\\') *q = '/';
  size_t len = strlen(tmp);
  while (len > 1 && tmp[len - 1] == '/') {
    if (len == 3 && tmp[1] == ':') break;  // preserve "C:/"
    tmp[--len] = 0;
  }
#if defined(_WIN32)
  struct __stat64 st{};
  if (_stat64(tmp, &st) != 0) return -1;
  if (is_dir) *is_dir = (st.st_mode & _S_IFDIR) ? 1 : 0;
  if (size_out) *size_out = (u64)st.st_size;
#else
  struct stat st{};
  if (stat(tmp, &st) != 0) return -1;
  if (is_dir) *is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
  if (size_out) *size_out = (u64)st.st_size;
#endif
  return 0;
}

int trt_vfs_ensure_parents(const char* host_path) {
  if (!host_path || !*host_path) return -1;
  char tmp[1024];
  snprintf(tmp, sizeof(tmp), "%s", host_path);
  for (char* q = tmp; *q; ++q)
    if (*q == '\\') *q = '/';
  char* p = tmp + 1;
  if (tmp[0] && tmp[1] == ':' && tmp[2] == '/') p = tmp + 3;
  for (; *p; ++p) {
    if (*p == '/') {
      *p = 0;
      if (*tmp) vfs_mkdir_one(tmp);
      *p = '/';
    }
  }
  return 0;
}

int trt_vfs_mkdir_p(const char* host_dir) {
  if (!host_dir || !*host_dir) return -1;
  char tmp[1024];
  snprintf(tmp, sizeof(tmp), "%s", host_dir);
  for (char* q = tmp; *q; ++q)
    if (*q == '\\') *q = '/';
  size_t len = strlen(tmp);
  while (len > 1 && tmp[len - 1] == '/') {
    if (len == 3 && tmp[1] == ':') break;
    tmp[--len] = 0;
  }
  trt_vfs_ensure_parents(tmp);
  if (vfs_mkdir_one(tmp) == 0) return 0;
  int is_dir = 0;
  if (errno == EEXIST || (trt_vfs_stat(tmp, &is_dir, nullptr) == 0 && is_dir))
    return 0;
  return -1;
}

static const vfs_mount* find_mount(const char* drv) {
  for (int i = 0; i < g_mount_count; ++i)
    if (strcmp(g_mounts[i].prefix, drv) == 0) return &g_mounts[i];
  return nullptr;
}

static void join_norm(char* out, size_t cap, const char* base, const char* sub,
                      const char* rest) {
  snprintf(out, cap, "%s%s%s%s", base, sub ? sub : "",
           (rest && *rest) ? "/" : "", rest ? rest : "");
  for (char* q = out; *q; ++q)
    if (*q == '\\') *q = '/';
}

// ---- disc images -----------------------------------------------------------
// A mounted image is a read-only drive. Guest paths under it are served from
// the image: stat/enumerate answer from the ISO records (real sizes, real
// names), and a file open extracts just that file into the cache once.

static vfs_image* image_find_locked(const char* drv) {
  for (int i = 0; i < g_image_count; ++i)
    if (strcmp(g_images[i].prefix, drv) == 0) return &g_images[i];
  return nullptr;
}

void trt_vfs_set_iso_cache_root(const char* host_dir) {
  if (!host_dir) return;
  vfs_lock();
  snprintf(g_iso_cache_root, sizeof(g_iso_cache_root), "%s", host_dir);
  vfs_unlock();
}

int trt_vfs_mount_image(const char* drive_prefix, const char* host_image) {
  if (!drive_prefix || !host_image) return 0;
  char norm_p[8] = {};
  for (size_t i = 0; i < 7 && drive_prefix[i]; ++i)
    norm_p[i] = lower_of(drive_prefix[i]);
  trt_iso* iso = trt_iso_open(host_image);
  if (!iso) return 0;
  vfs_lock();
  int slot = -1;
  for (int i = 0; i < g_image_count; ++i)
    if (strcmp(g_images[i].prefix, norm_p) == 0) { slot = i; break; }
  if (slot < 0) {
    if (g_image_count >= 8) {
      vfs_unlock();
      trt_iso_close(iso);
      return 0;
    }
    slot = g_image_count++;
  } else if (g_images[slot].iso) {
    trt_iso_close(g_images[slot].iso);
  }
  memset(&g_images[slot], 0, sizeof(vfs_image));
  strncpy(g_images[slot].prefix, norm_p, sizeof(g_images[slot].prefix) - 1);
  strncpy(g_images[slot].image_path, host_image,
          sizeof(g_images[slot].image_path) - 1);
  g_images[slot].iso = iso;
  vfs_unlock();
  return 1;
}

void trt_vfs_unmount_image(const char* drive_prefix) {
  if (!drive_prefix) return;
  char norm_p[8] = {};
  for (size_t i = 0; i < 7 && drive_prefix[i]; ++i)
    norm_p[i] = lower_of(drive_prefix[i]);
  vfs_lock();
  vfs_image* im = image_find_locked(norm_p);
  if (im) {
    trt_iso_close(im->iso);
    memset(im, 0, sizeof(*im));
  }
  vfs_unlock();
}

// Resolve a guest path to (image, in-image path). Returns the image or null.
static trt_iso* image_lookup_locked(const char* guest_utf8, char* rel_out,
                                    size_t rel_cap) {
  if (!guest_utf8 || g_image_count == 0) return nullptr;
  char drv[3] = {}, rest[1024] = "";
  if (normalize_guest_locked(guest_utf8, drv, rest, sizeof(rest), nullptr, 0,
                             false) != 0)
    return nullptr;
  vfs_image* im = image_find_locked(drv);
  if (!im || !im->iso) return nullptr;
  if (rel_out && rel_cap) snprintf(rel_out, rel_cap, "/%s", rest);
  return im->iso;
}

trt_iso* trt_vfs_image_for_guest_utf16(const u16* guest_path, char* rel_out,
                                       size_t rel_cap) {
  if (!guest_path || !guest_path[0]) return nullptr;
  char utf8[1024];
  utf16_to_utf8(guest_path, utf8, sizeof(utf8));
  vfs_lock();
  trt_iso* iso = image_lookup_locked(utf8, rel_out, rel_cap);
  vfs_unlock();
  return iso;
}

int trt_vfs_is_image_utf16(const u16* guest_path) {
  return trt_vfs_image_for_guest_utf16(guest_path, nullptr, 0) != nullptr;
}

// Sanitize one image path segment for the host cache (Windows-illegal chars).
static void cache_sanitize(const char* in, char* out, size_t cap) {
  size_t o = 0;
  for (size_t i = 0; in[i] && o + 1 < cap; ++i) {
    const unsigned char c = (unsigned char)in[i];
    if (c < 0x20 || c == '<' || c == '>' || c == ':' || c == '"' || c == '|' ||
        c == '?' || c == '*' || c == '\\')
      out[o++] = '_';
    else
      out[o++] = (char)c;
  }
  out[o] = 0;
}

// Make the image path available as a host path. Directories are created;
// files are extracted once (a second call reuses the cached copy).
// Returns 0 when the path exists in the image, -1 when it does not.
int trt_vfs_image_materialize(const u16* guest_path, char* host_out,
                              size_t host_cap, int* is_dir_out) {
  if (!guest_path || !host_out || host_cap == 0) return -1;
  char rel[1024] = "";
  trt_iso* iso = trt_vfs_image_for_guest_utf16(guest_path, rel, sizeof(rel));
  if (!iso) return -1;

  int is_dir = 0;
  u64 size = 0;
  if (trt_iso_stat(iso, rel, &is_dir, &size) != 0) return -1;
  if (is_dir_out) *is_dir_out = is_dir;

  char root[512];
  vfs_lock();
  snprintf(root, sizeof(root), "%s", g_iso_cache_root);
  char drv[8] = "d:";
  {
    char utf8[1024];
    utf16_to_utf8(guest_path, utf8, sizeof(utf8));
    char d2[3] = {}, r2[1024] = "";
    if (normalize_guest_locked(utf8, d2, r2, sizeof(r2), nullptr, 0, false) == 0 &&
        d2[0])
      snprintf(drv, sizeof(drv), "%c", lower_of(d2[0]));
  }
  vfs_unlock();

  if (!root[0]) {
    // no cache root configured: fall back next to the image
    snprintf(root, sizeof(root), "/tmp/tiramisu-iso-cache");
  }
  char safe[1024];
  cache_sanitize(rel, safe, sizeof(safe));
  char path[1600];
  snprintf(path, sizeof(path), "%s/%c%s", root, drv[0], safe);
  for (char* q = path; *q; ++q)
    if (*q == '\\') *q = '/';

  if (is_dir) {
    trt_vfs_mkdir_p(path);
    snprintf(host_out, host_cap, "%s", path);
    return 0;
  }
  trt_vfs_ensure_parents(path);
  int cached_dir = 0;
  u64 cached_size = 0;
  if (trt_vfs_stat(path, &cached_dir, &cached_size) == 0 && !cached_dir &&
      cached_size == size) {
    snprintf(host_out, host_cap, "%s", path);
    return 0;
  }
  if (trt_iso_extract(iso, rel, path) != 0) return -1;
  snprintf(host_out, host_cap, "%s", path);
  return 0;
}

int trt_vfs_resolve_utf16_rw(const u16* guest_path, int for_write, char* out,
                             size_t out_cap) {
  if (!guest_path || !out || out_cap == 0) return -1;
  char utf8[1024];
  utf16_to_utf8(guest_path, utf8, sizeof(utf8));
  const char* p = utf8;
  // Strip Win32 extended-length / NT device prefixes ("\\?\", "\??\", "\\.\")
  // that engines like Godot FileAccessWindows::fix_path prepend.
  if (((p[0] == '\\' || p[0] == '/') && (p[1] == '\\' || p[1] == '/') &&
       (p[2] == '?' || p[2] == '.') && (p[3] == '\\' || p[3] == '/')) ||
      (p[0] == '\\' && p[1] == '?' && p[2] == '?' && p[3] == '\\')) {
    p += 4;
  }
  vfs_lock();
  char drv[3] = {}, rest[1024] = {};
  if (normalize_guest_locked(utf8, drv, rest, sizeof(rest), nullptr, 0, false) != 0) {
    vfs_unlock();
    return -1;
  }
  // A mounted disc image answers every path itself: materialize (extract once)
  // and hand the host cache path to the caller. Writes are refused upstream.
  if (image_find_locked(drv) != nullptr) {
    vfs_unlock();
    int is_dir = 0;
    return trt_vfs_image_materialize(guest_path, out, out_cap, &is_dir);
  }
  const vfs_mount* m = find_mount(drv);
  if (!m) {
    vfs_unlock();
    return -1;
  }
  if (strcmp(drv, "z:") == 0) {
    const vfs_mount* cm = find_mount("c:");
    if (cm) {
      ensure_whiteouts_loaded_locked(cm->target);
      char ov[1024], zpath[1024], rel_lower[1024];
      to_lower_rel(rest, rel_lower, sizeof(rel_lower));
      join_norm(ov, sizeof(ov), cm->target, "/_z_write", rest);
      if (for_write) {
        vfs_unlock();
        snprintf(out, out_cap, "%s", ov);
        return 0;
      }
      join_norm(zpath, sizeof(zpath), m->target, "", rest);
      int ov_dir = 0;
      const bool ov_exists = (trt_vfs_stat(ov, &ov_dir, nullptr) == 0);
      const bool whited_out = is_rel_whited_out_locked(rel_lower, ov_exists);
      vfs_unlock();
      if (whited_out) {
        // Point at absent ov path (or cleaned ov) so stat/open returns ENOENT
        snprintf(out, out_cap, "%s", ov);
        return 0;
      }
      if (ov_exists) {
        if (!ov_dir || (*rest != '\0' && trt_vfs_stat(zpath, nullptr, nullptr) != 0)) {
          snprintf(out, out_cap, "%s", ov);
          return 0;
        }
      }
      snprintf(out, out_cap, "%s", zpath);
      return 0;
    }
  }
  vfs_unlock();
  join_norm(out, out_cap, m->target, "", rest);
  return 0;
}

void trt_vfs_set_cwd(const char* guest_dir) {
  if (!guest_dir || !guest_dir[0]) return;
  vfs_lock();
  char drv[3] = {}, rel[1024] = {}, norm_guest[512] = "";
  if (normalize_guest_locked(guest_dir, drv, rel, sizeof(rel), norm_guest,
                             sizeof(norm_guest), false) == 0) {
    g_cwd_drive[0] = drv[0];
    g_cwd_drive[1] = ':';
    g_cwd_drive[2] = 0;
    snprintf(g_cwd_guest, sizeof(g_cwd_guest), "%s", norm_guest);
  }
  vfs_unlock();
}

int trt_vfs_get_cwd(char* out, size_t out_cap) {
  if (!out || out_cap == 0) return -1;
  vfs_lock();
  char drv[3] = {}, rel[512] = "";
  effective_cwd_locked(drv, rel, sizeof(rel));
  if (rel[0]) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%c:\\%s", upper_of(drv[0]), rel);
    for (char* q = tmp; *q; ++q) if (*q == '/') *q = '\\';
    snprintf(out, out_cap, "%s", tmp);
  } else {
    snprintf(out, out_cap, "%c:\\", upper_of(drv[0]));
  }
  vfs_unlock();
  return 0;
}

int trt_vfs_normalize_guest_utf16(const u16* guest_path, char* out,
                                  size_t out_cap) {
  if (!guest_path || !guest_path[0] || !out || out_cap == 0) return -1;
  char utf8[1024];
  utf16_to_utf8(guest_path, utf8, sizeof(utf8));
  vfs_lock();
  char drv[3] = {}, rel[1024] = "";
  int rc = normalize_guest_locked(utf8, drv, rel, sizeof(rel), out, out_cap, true);
  vfs_unlock();
  return rc;
}

int trt_vfs_resolve_z_pair_utf16(const u16* guest_path, char* ov_out,
                                 size_t ov_cap, char* ro_out, size_t ro_cap,
                                 char* rel_out, size_t rel_cap) {
  if (!guest_path || !guest_path[0]) return -1;
  char utf8[1024];
  utf16_to_utf8(guest_path, utf8, sizeof(utf8));
  vfs_lock();
  char drv[3] = {}, rest[1024] = "";
  if (normalize_guest_locked(utf8, drv, rest, sizeof(rest), nullptr, 0, false) != 0) {
    vfs_unlock();
    return -1;
  }
  if (strcmp(drv, "z:") != 0) {
    vfs_unlock();
    return -1;
  }
  const vfs_mount* zm = find_mount("z:");
  const vfs_mount* cm = find_mount("c:");
  if (!zm) {
    vfs_unlock();
    return -1;
  }
  if (cm) ensure_whiteouts_loaded_locked(cm->target);
  if (ov_out && ov_cap) {
    if (cm) join_norm(ov_out, ov_cap, cm->target, "/_z_write", rest);
    else ov_out[0] = 0;
  }
  if (ro_out && ro_cap) join_norm(ro_out, ro_cap, zm->target, "", rest);
  if (rel_out && rel_cap) to_lower_rel(rest, rel_out, rel_cap);
  vfs_unlock();
  return 0;
}

int trt_vfs_is_z_deleted_rel(const char* z_rel, int is_child) {
  if (!z_rel || !z_rel[0]) return 0;
  char rel[1024];
  to_lower_rel(z_rel, rel, sizeof(rel));
  if (!rel[0]) return 0;
  vfs_lock();
  const vfs_mount* cm = find_mount("c:");
  if (cm) ensure_whiteouts_loaded_locked(cm->target);
  bool del = vec_has(&g_z_deleted, rel);
  if (!del && !is_child) {
    del = is_rel_whited_out_locked(rel, false);
  }
  if (!del && is_child) {
    // check ancestors if caller passes a direct child under ro dir
    char anc[1024];
    snprintf(anc, sizeof(anc), "%s", rel);
    for (;;) {
      char* slash = strrchr(anc, '/');
      if (!slash) break;
      *slash = 0;
      if (!anc[0]) break;
      if (vec_has(&g_z_deleted, anc)) { del = true; break; }
    }
  }
  vfs_unlock();
  return del ? 1 : 0;
}

int trt_vfs_is_z_opaque_rel(const char* z_rel) {
  if (!z_rel || !z_rel[0]) return 0;
  char rel[1024];
  to_lower_rel(z_rel, rel, sizeof(rel));
  if (!rel[0]) return 0;
  vfs_lock();
  const vfs_mount* cm = find_mount("c:");
  if (cm) ensure_whiteouts_loaded_locked(cm->target);
  bool opq = is_dir_opaque_locked(rel);
  vfs_unlock();
  return opq ? 1 : 0;
}

void trt_vfs_mark_z_deleted_utf16(const u16* guest_path, int is_deleted,
                                  int is_dir) {
  if (!guest_path || !guest_path[0]) return;
  char utf8[1024];
  utf16_to_utf8(guest_path, utf8, sizeof(utf8));
  vfs_lock();
  char drv[3] = {}, rest[1024] = "";
  if (normalize_guest_locked(utf8, drv, rest, sizeof(rest), nullptr, 0, false) != 0 ||
      strcmp(drv, "z:") != 0) {
    vfs_unlock();
    return;
  }
  const vfs_mount* cm = find_mount("c:");
  if (!cm) {
    vfs_unlock();
    return;
  }
  ensure_whiteouts_loaded_locked(cm->target);
  char rel[1024];
  to_lower_rel(rest, rel, sizeof(rel));
  if (!rel[0]) {
    vfs_unlock();
    return;
  }
  if (is_deleted) {
    if (is_dir) {
      vec_remove_tree(&g_z_deleted, rel);
      vec_remove_tree(&g_z_opaque, rel);
    }
    vec_add(&g_z_deleted, rel);
  } else {
    vec_remove_exact(&g_z_deleted, rel);
    if (is_dir) {
      vec_remove_tree(&g_z_deleted, rel);
      vec_add(&g_z_opaque, rel);
    } else {
      vec_remove_exact(&g_z_opaque, rel);
    }
    // Promote any deleted ancestor directories into opaque directories
    char anc[1024];
    snprintf(anc, sizeof(anc), "%s", rel);
    for (;;) {
      char* slash = strrchr(anc, '/');
      if (!slash) break;
      *slash = 0;
      if (!anc[0]) break;
      if (vec_has(&g_z_deleted, anc)) {
        vec_remove_exact(&g_z_deleted, anc);
        vec_add(&g_z_opaque, anc);
      }
    }
  }
  persist_whiteouts_locked(cm->target);
  vfs_unlock();
}

static bool host_dir_has_non_dot(const char* host_dir, const char* rel_lower,
                                 bool check_z_ro) {
#if defined(_WIN32)
  char pat[1100];
  snprintf(pat, sizeof(pat), "%s\\*", host_dir);
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(pat, &fd);
  if (h == INVALID_HANDLE_VALUE) return false;
  bool found_any = false;
  do {
    if (fd.cFileName[0] == '.' &&
        (fd.cFileName[1] == 0 || (fd.cFileName[1] == '.' && fd.cFileName[2] == 0)))
      continue;
    if (!check_z_ro) { found_any = true; break; }
    char child[1024];
    if (rel_lower && rel_lower[0])
      snprintf(child, sizeof(child), "%s/%s", rel_lower, fd.cFileName);
    else
      snprintf(child, sizeof(child), "%s", fd.cFileName);
    if (!trt_vfs_is_z_deleted_rel(child, 1)) { found_any = true; break; }
  } while (FindNextFileA(h, &fd));
  FindClose(h);
  return found_any;
#else
  DIR* d = opendir(host_dir);
  if (!d) return false;
  bool found_any = false;
  for (;;) {
    dirent* e = readdir(d);
    if (!e) break;
    if (e->d_name[0] == '.' &&
        (e->d_name[1] == 0 || (e->d_name[1] == '.' && e->d_name[2] == 0)))
      continue;
    if (!check_z_ro) { found_any = true; break; }
    char child[1024];
    if (rel_lower && rel_lower[0])
      snprintf(child, sizeof(child), "%s/%s", rel_lower, e->d_name);
    else
      snprintf(child, sizeof(child), "%s", e->d_name);
    if (!trt_vfs_is_z_deleted_rel(child, 1)) { found_any = true; break; }
  }
  closedir(d);
  return found_any;
#endif
}

int trt_vfs_delete_file_utf16(const u16* guest_path, u32* win_err_out) {
  if (win_err_out) *win_err_out = 0;
  if (!guest_path || !guest_path[0]) {
    if (win_err_out) *win_err_out = 87;
    return -1;
  }
  char ov[1024] = "", ro[1024] = "", rel[1024] = "";
  if (trt_vfs_resolve_z_pair_utf16(guest_path, ov, sizeof(ov),
                                   ro, sizeof(ro), rel, sizeof(rel)) == 0) {
    if (!rel[0]) { if (win_err_out) *win_err_out = 5; return -1; }
    int ov_dir = 0, ro_dir = 0;
    const bool ov_exists = ov[0] && trt_vfs_stat(ov, &ov_dir, nullptr) == 0;
    const bool ro_exists = ro[0] && trt_vfs_stat(ro, &ro_dir, nullptr) == 0;
    vfs_lock();
    const bool whited = is_rel_whited_out_locked(rel, ov_exists);
    vfs_unlock();
    if ((!ov_exists && (!ro_exists || whited)) || (whited && !ov_exists)) {
      if (win_err_out) *win_err_out = 2;
      return -1;
    }
    if ((ov_exists && ov_dir) || (!ov_exists && ro_exists && ro_dir)) {
      if (win_err_out) *win_err_out = 5;
      return -1;
    }
    if (ov_exists && remove(ov) != 0) {
      if (win_err_out) *win_err_out = 5;
      return -1;
    }
    if (ro_exists) trt_vfs_mark_z_deleted_utf16(guest_path, 1, 0);
    return 0;
  }
  char host[1024];
  if (trt_vfs_resolve_utf16_rw(guest_path, 1, host, sizeof(host)) != 0) {
    utf16_to_utf8(guest_path, host, sizeof(host));
  }
  int is_dir = 0;
  if (trt_vfs_stat(host, &is_dir, nullptr) != 0) {
    if (win_err_out) *win_err_out = 2;
    return -1;
  }
  if (is_dir) {
    if (win_err_out) *win_err_out = 5;
    return -1;
  }
  if (remove(host) != 0) {
    if (win_err_out) *win_err_out = 5;
    return -1;
  }
  return 0;
}

int trt_vfs_remove_dir_utf16(const u16* guest_path, u32* win_err_out) {
  if (win_err_out) *win_err_out = 0;
  if (!guest_path || !guest_path[0]) {
    if (win_err_out) *win_err_out = 87;
    return -1;
  }
  char ov[1024] = "", ro[1024] = "", rel[1024] = "";
  if (trt_vfs_resolve_z_pair_utf16(guest_path, ov, sizeof(ov),
                                   ro, sizeof(ro), rel, sizeof(rel)) == 0) {
    if (!rel[0]) { if (win_err_out) *win_err_out = 5; return -1; }
    int ov_dir = 0, ro_dir = 0;
    const bool ov_exists = ov[0] && trt_vfs_stat(ov, &ov_dir, nullptr) == 0;
    const bool ro_exists = ro[0] && trt_vfs_stat(ro, &ro_dir, nullptr) == 0;
    vfs_lock();
    const bool whited = is_rel_whited_out_locked(rel, ov_exists);
    const bool opaque = is_dir_opaque_locked(rel);
    vfs_unlock();
    if (!ov_exists && (!ro_exists || whited)) {
      if (win_err_out) *win_err_out = 2;
      return -1;
    }
    if ((ov_exists && !ov_dir) || (!ov_exists && ro_exists && !ro_dir)) {
      if (win_err_out) *win_err_out = 267;  // ERROR_DIRECTORY
      return -1;
    }
    if (ov_exists && host_dir_has_non_dot(ov, rel, false)) {
      if (win_err_out) *win_err_out = 145;  // ERROR_DIR_NOT_EMPTY
      return -1;
    }
    if (ro_exists && !opaque && host_dir_has_non_dot(ro, rel, true)) {
      if (win_err_out) *win_err_out = 145;
      return -1;
    }
    if (ov_exists && vfs_rmdir_one(ov) != 0) {
      if (win_err_out) *win_err_out = 5;
      return -1;
    }
    if (ro_exists) trt_vfs_mark_z_deleted_utf16(guest_path, 1, 1);
    return 0;
  }
  char host[1024];
  if (trt_vfs_resolve_utf16_rw(guest_path, 1, host, sizeof(host)) != 0) {
    utf16_to_utf8(guest_path, host, sizeof(host));
  }
  if (vfs_rmdir_one(host) != 0) {
    if (win_err_out) *win_err_out = (errno == ENOTEMPTY || errno == EEXIST) ? 145u : 2u;
    return -1;
  }
  return 0;
}

// translate "C:\foo\bar" -> "<host>/foo/bar", and relative paths like
// ".\_MEI42422\file" or "sub\\file" -> "<cwd-drive>/foo/bar".
// Returns 0 on hit, -1 no mount.
int trt_vfs_resolve_utf16(const u16* guest_path, char* out, size_t out_cap) {
  return trt_vfs_resolve_utf16_rw(guest_path, 0, out, out_cap);
}

} // extern "C"
