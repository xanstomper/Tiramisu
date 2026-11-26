// tiramisu/src/trt/iso.cpp
// Disc-image reader: ISO9660 (+Joliet +Rock Ridge names) and UDF 1.02/2.01,
// over 2048-byte cooked sectors or raw 2352/2336 dumps.
//
// Why this exists: a large share of PC games ship on a disc (or as a rip of
// one). Their launcher asks for D:\data\... and the disc must answer with the
// real bytes. Extraction happens lazily, one file at a time, into the VFS
// cache - the image is never copied wholesale.
//
// Evidence: tests/test_iso.cpp builds real ISO9660+Joliet and UDF images in
// memory, mounts them, and reads a file back through the same API the guest
// path uses.
#include "iso.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  define iso_seek(f, o) _fseeki64((f), (o), SEEK_SET)
#  define iso_tell(f) _ftelli64(f)
#  define iso_size(f) (_fseeki64((f), 0, SEEK_END), _ftelli64(f))
#else
#  include <dirent.h>
#  include <strings.h>
#  include <sys/stat.h>
#  define iso_seek(f, o) fseeko((f), (off_t)(o), SEEK_SET)
#  define iso_tell(f) ((int64_t)ftello(f))
#  define iso_size(f) (fseeko((f), 0, SEEK_END), (int64_t)ftello(f))
#endif

namespace {

constexpr uint32_t k_block = 2048;

struct extent {
  uint64_t lba;    // logical block address (in image blocks)
  uint64_t len;    // bytes
};

struct iso_node {
  std::string name;             // UTF-8, display name
  bool is_dir = false;
  uint64_t size = 0;
  std::vector<extent> ext;      // file extents (empty for dirs)
  std::vector<iso_node*> kids;  // dir children
  iso_node* parent = nullptr;
  uint32_t dir_lba = 0;         // where the directory records live
  uint64_t dir_len = 0;
};

char lower_c(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

std::string lower_s(const std::string& s) {
  std::string o = s;
  for (char& c : o) c = lower_c(c);
  return o;
}

uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
uint16_t rd16be(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
uint32_t rd32be(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
uint64_t rd64be(const uint8_t* p) {
  return ((uint64_t)rd32be(p) << 32) | rd32be(p + 4);
}

// UCS-2BE (Joliet) -> UTF-8
std::string utf16be_to_utf8(const uint8_t* p, size_t bytes) {
  std::string out;
  for (size_t i = 0; i + 1 < bytes; i += 2) {
    uint32_t cp = rd16be(p + i);
    if (cp == 0) continue;
    if (cp < 0x80) {
      out.push_back((char)cp);
    } else if (cp < 0x800) {
      out.push_back((char)(0xC0 | (cp >> 6)));
      out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
      out.push_back((char)(0xE0 | (cp >> 12)));
      out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back((char)(0x80 | (cp & 0x3F)));
    }
  }
  return out;
}

// OSTA CS0 compressed unicode (UDF file identifiers) -> UTF-8
std::string osta_to_utf8(const uint8_t* p, size_t n) {
  if (n == 0) return std::string();
  const uint8_t comp = p[0];
  std::string out;
  if (comp == 8) {
    for (size_t i = 1; i < n; ++i) {
      uint32_t cp = p[i];
      if (cp == 0) continue;
      if (cp < 0x80) out.push_back((char)cp);
      else {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
      }
    }
  } else if (comp == 16) {
    for (size_t i = 1; i + 1 < n; i += 2) {
      uint32_t cp = rd16be(p + i);
      if (cp == 0) continue;
      if (cp < 0x80) out.push_back((char)cp);
      else if (cp < 0x800) {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
      } else {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
      }
    }
  } else {
    // Uncompressed: the whole field is the name (rare, but legal).
    for (size_t i = 0; i < n; ++i) {
      uint32_t cp = p[i];
      if (!cp) continue;
      if (cp < 0x80) out.push_back((char)cp);
      else {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
      }
    }
  }
  return out;
}

// ISO9660 file identifiers are uppercase ASCII with ";1" version suffixes and
// a trailing '.' for extension-less files. Clean them for display.
std::string iso_name_clean(const uint8_t* p, size_t n) {
  std::string s((const char*)p, n);
  const size_t semi = s.find(';');
  if (semi != std::string::npos) s.resize(semi);
  while (!s.empty() && s.back() == '.') s.pop_back();
  return s;
}

struct sector_map {
  uint32_t stride = 2048;
  uint32_t user_off = 0;
};

}  // namespace

struct trt_iso {
  FILE* f = nullptr;
  sector_map sm;
  uint64_t image_bytes = 0;
  iso_node* root = nullptr;
  bool joliet = false;
  bool udf = false;
  bool iso9660 = false;
  uint32_t udf_block = 2048;
  uint32_t udf_part_start = 0;   // partition start LBA (in udf blocks)
  uint32_t file_count = 0;
  uint32_t dir_count = 0;
};

namespace {

// ---- image byte access -----------------------------------------------------
bool rd_at(trt_iso* iso, uint64_t off, void* buf, size_t len) {
  if (!iso->f) return false;
  if (iso_seek(iso->f, off) != 0) return false;
  return fread(buf, 1, len, iso->f) == len;
}

// Read one 2048-byte logical sector (LBA) out of the image, honouring the
// sector stride of a raw dump.
bool rd_sector(trt_iso* iso, uint64_t lba, void* out) {
  const uint64_t off = lba * iso->sm.stride + iso->sm.user_off;
  if (off + k_block > iso->image_bytes) return false;
  return rd_at(iso, off, out, k_block);
}

// ---- node tree -------------------------------------------------------------
iso_node* node_new(iso_node* parent, const std::string& name) {
  iso_node* n = new iso_node();
  n->name = name;
  n->parent = parent;
  return n;
}

void node_free(iso_node* n) {
  if (!n) return;
  for (iso_node* k : n->kids) node_free(k);
  delete n;
}

iso_node* dir_find(iso_node* dir, const std::string& name) {
  if (!dir) return nullptr;
  const std::string want = lower_s(name);
  for (iso_node* k : dir->kids)
    if (lower_s(k->name) == want) return k;
  return nullptr;
}

// ---- ISO9660 ---------------------------------------------------------------
// Parse a directory extent into the node tree. Multi-extent files appear as
// consecutive records with the "not final" flag; they are concatenated.
bool parse_iso_dir(trt_iso* iso, iso_node* dir, uint32_t lba, uint64_t len,
                   bool joliet) {
  if (!dir) return false;
  dir->is_dir = true;
  dir->dir_lba = lba;
  dir->dir_len = len;
  std::vector<uint8_t> data((size_t)len);
  // Directory extents are contiguous in cooked images; read sector by sector
  // so raw dumps work too.
  size_t got = 0;
  uint64_t cur = lba;
  while (got < len) {
    uint8_t sec[k_block];
    if (!rd_sector(iso, cur, sec)) break;
    size_t take = (size_t)std::min<uint64_t>(k_block, len - got);
    memcpy(data.data() + got, sec, take);
    got += take;
    ++cur;
  }
  if (got < len) data.resize(got);

  size_t p = 0;
  iso_node* pending = nullptr;  // multi-extent accumulation
  while (p + 33 <= data.size()) {
    const uint8_t rec_len = data[p];
    if (rec_len == 0) {
      // records never cross a sector: skip to the next one
      const size_t next = ((p / k_block) + 1) * k_block;
      if (next <= p) break;
      p = next;
      continue;
    }
    if (p + rec_len > data.size()) break;
    const uint8_t* r = data.data() + p;
    const uint32_t ext_lba = rd32(r + 2);
    const uint32_t ext_len = rd32(r + 10);
    const uint8_t flags = r[25];
    const uint8_t name_len = r[32];
    const uint8_t* name_p = r + 33;
    const bool is_dir = (flags & 0x02) != 0;
    const bool multi = (flags & 0x80) != 0;

    std::string name;
    if (name_len == 1 && name_p[0] == 0) name = ".";
    else if (name_len == 1 && name_p[0] == 1) name = "..";
    else if (joliet) name = utf16be_to_utf8(name_p, name_len);
    else name = iso_name_clean(name_p, name_len);

    p += rec_len;
    if (name.empty() || name == "." || name == "..") continue;

    if (!is_dir && multi) {
      if (!pending) {
        pending = node_new(dir, name);
        dir->kids.push_back(pending);
        iso->file_count++;
      }
      pending->ext.push_back({ext_lba, ext_len});
      pending->size += ext_len;
      continue;
    }
    if (pending) {
      if (!is_dir && lower_s(pending->name) == lower_s(name)) {
        pending->ext.push_back({ext_lba, ext_len});
        pending->size += ext_len;
        pending = nullptr;
        continue;
      }
      pending = nullptr;
    }
    if (dir_find(dir, name)) continue;  // Joliet + RR duplicates
    iso_node* n = node_new(dir, name);
    n->is_dir = is_dir;
    dir->kids.push_back(n);
    if (is_dir) {
      iso->dir_count++;
      parse_iso_dir(iso, n, ext_lba, ext_len, joliet);
    } else {
      iso->file_count++;
      n->size = ext_len;
      n->ext.push_back({ext_lba, ext_len});
    }
  }
  return true;
}

bool open_iso9660(trt_iso* iso) {
  uint8_t sec[k_block];
  if (!rd_sector(iso, 16, sec)) return false;
  if (memcmp(sec + 1, "CD001", 5) != 0) return false;

  uint8_t pvd[k_block];
  bool have_pvd = false;
  bool have_joliet = false;
  uint8_t joliet_vd[k_block];
  for (uint32_t lba = 16; lba < 16 + 32; ++lba) {
    uint8_t vd[k_block];
    if (!rd_sector(iso, lba, vd)) break;
    const uint8_t type = vd[0];
    if (memcmp(vd + 1, "CD001", 5) != 0) break;
    if (type == 1 && !have_pvd) {
      memcpy(pvd, vd, k_block);
      have_pvd = true;
    } else if (type == 2 && !have_joliet) {
      // Joliet escape sequences: %/@, %/C, %/E
      if (vd[88] == '%' && vd[89] == '/' &&
          (vd[90] == '@' || vd[90] == 'C' || vd[90] == 'E')) {
        memcpy(joliet_vd, vd, k_block);
        have_joliet = true;
      }
    } else if (type == 255) {
      break;
    }
  }
  if (!have_pvd) return false;

  const uint8_t* use = have_joliet ? joliet_vd : pvd;
  iso->joliet = have_joliet;
  iso->iso9660 = true;

  // Root directory record lives at offset 156 of the volume descriptor.
  const uint8_t* root = use + 156;
  if (root[0] < 34) return false;
  const uint32_t root_lba = rd32(root + 2);
  const uint32_t root_len = rd32(root + 10);
  iso->root = node_new(nullptr, "/");
  if (!parse_iso_dir(iso, iso->root, root_lba, root_len, have_joliet))
    return false;
  return true;
}

// ---- UDF -------------------------------------------------------------------
struct udf_ad {
  uint32_t type;
  uint32_t len;
  uint32_t loc;
  uint16_t part;
};

// Parse one allocation descriptor at p (short_ad or long_ad), advancing *used.
udf_ad parse_ad(const uint8_t* p, bool long_form, size_t* used) {
  udf_ad a{};
  a.len = rd32(p) & 0x3FFFFFFF;
  a.type = (rd32(p) >> 30) & 3;
  a.loc = rd32(p + 4);
  a.part = long_form ? rd16(p + 8) : 0;
  *used = long_form ? 16 : 8;
  return a;
}

bool udf_read_block(trt_iso* iso, uint32_t block, void* out) {
  return rd_sector(iso, (uint64_t)block, out);
}

// Build a node from a UDF File Entry (ICB). Returns nullptr on a bad tag.
iso_node* udf_build(trt_iso* iso, iso_node* parent, const std::string& name,
                    uint32_t icb_block, int depth);

bool udf_parse_dir(trt_iso* iso, iso_node* dir, uint32_t icb_block, int depth) {
  uint8_t fe[k_block];
  if (!udf_read_block(iso, icb_block, fe)) return false;
  const uint16_t tag = rd16(fe);
  if (tag != 261 && tag != 266) return false;  // FE / EFE
  const bool efe = (tag == 266);
  const uint8_t file_type = fe[27];
  if (file_type != 4) return false;

  const uint64_t info_len = efe ? rd64be(fe + 56) : rd64be(fe + 56);
  const uint32_t alloc_len_off = efe ? 180u : 172u;
  const uint32_t alloc_off = efe ? 184u : 176u;
  const uint32_t alloc_len = rd32(fe + alloc_len_off);

  // Directory data may be split across several extents; read them in order.
  std::vector<uint8_t> data;
  data.reserve((size_t)std::min<uint64_t>(info_len, 1u << 22));
  size_t p = alloc_off;
  const size_t end = alloc_off + alloc_len;
  while (p + 8 <= end && p + 8 <= k_block) {
    size_t used = 0;
    udf_ad a = parse_ad(fe + p, true, &used);  // UDF FEs use long_ad
    p += used;
    if (a.type == 3) continue;  // next extent of descriptors: not handled
    if (a.type != 0 || a.len == 0) continue;
    std::vector<uint8_t> chunk(a.len);
    size_t got = 0;
    uint32_t cur = a.loc;
    while (got < a.len) {
      uint8_t blk[k_block];
      if (!udf_read_block(iso, cur, blk)) break;
      const size_t take = std::min<size_t>(k_block, a.len - got);
      memcpy(chunk.data() + got, blk, take);
      got += take;
      ++cur;
    }
    chunk.resize(got);
    data.insert(data.end(), chunk.begin(), chunk.end());
  }

  size_t q = 0;
  while (q + 38 <= data.size()) {
    const uint8_t* fid = data.data() + q;
    const uint16_t ftag = rd16(fid);
    if (ftag != 257) break;  // FID
    const uint8_t chars = fid[18];
    const uint8_t id_len = fid[19];
    const uint32_t icb_loc = rd32(fid + 20 + 4);  // long_ad loc
    const uint16_t impl_len = rd16(fid + 36);
    const size_t rec_len = (38u + impl_len + id_len + 3u) & ~3u;
    if (rec_len < 38 || q + rec_len > data.size()) break;
    const uint8_t* name_p = fid + 38 + impl_len;
    const bool is_parent = (chars & 0x08) != 0;
    const bool is_deleted = (chars & 0x04) != 0;
    if (!is_parent && !is_deleted && id_len > 0) {
      std::string nm = osta_to_utf8(name_p, id_len);
      if (!nm.empty() && !dir_find(dir, nm)) {
        iso_node* n = udf_build(iso, dir, nm, icb_loc, depth + 1);
        if (n) dir->kids.push_back(n);
      }
    }
    q += rec_len;
  }
  return true;
}

iso_node* udf_build(trt_iso* iso, iso_node* parent, const std::string& name,
                    uint32_t icb_block, int depth) {
  if (depth > 48) return nullptr;
  uint8_t fe[k_block];
  if (!udf_read_block(iso, icb_block, fe)) return nullptr;
  const uint16_t tag = rd16(fe);
  if (tag != 261 && tag != 266) return nullptr;
  const bool efe = (tag == 266);
  const uint8_t file_type = fe[27];
  if (file_type != 4 && file_type != 5) return nullptr;

  iso_node* n = node_new(parent, name);
  n->is_dir = (file_type == 4);
  if (n->is_dir) {
    iso->dir_count++;
    udf_parse_dir(iso, n, icb_block, depth);
    return n;
  }
  iso->file_count++;
  const uint64_t info_len = rd64be(fe + 56);
  const uint32_t alloc_len_off = efe ? 180u : 172u;
  const uint32_t alloc_off = efe ? 184u : 176u;
  const uint32_t alloc_len = rd32(fe + alloc_len_off);
  n->size = info_len;
  size_t p = alloc_off;
  const size_t end = std::min<size_t>(alloc_off + alloc_len, k_block);
  while (p + 8 <= end) {
    size_t used = 0;
    udf_ad a = parse_ad(fe + p, true, &used);
    p += used;
    if (a.type == 0 && a.len) n->ext.push_back({(uint64_t)a.loc, a.len});
    // types 1/2 are "allocated but not recorded"/"unallocated" -> zeros
  }
  return n;
}

bool open_udf(trt_iso* iso) {
  // Anchor Volume Descriptor Pointer lives at block 256 (and N-256/N-1).
  uint8_t avdp[k_block];
  if (!udf_read_block(iso, 256, avdp)) return false;
  if (rd16(avdp) != 2) return false;  // AVDP tag

  // main volume descriptor sequence
  const uint32_t vds_len = rd32(avdp + 16);
  const uint32_t vds_loc = rd32(avdp + 20);
  if (!vds_loc || !vds_len) return false;

  uint32_t part_start = 0;
  uint32_t udf_block = k_block;
  uint32_t fsd_loc = 0;
  bool have_lvd = false;
  const uint32_t blocks = std::min<uint32_t>(vds_len / k_block + 1, 64);
  for (uint32_t i = 0; i < blocks; ++i) {
    uint8_t vd[k_block];
    if (!udf_read_block(iso, vds_loc + i, vd)) break;
    const uint16_t tag = rd16(vd);
    if (tag == 8) break;  // terminating descriptor
    if (tag == 5) {       // partition descriptor
      part_start = rd32(vd + 188);
    } else if (tag == 6) {  // logical volume descriptor
      udf_block = rd32(vd + 212);
      fsd_loc = rd32(vd + 248 + 4);   // long_ad location
      have_lvd = true;
    }
  }
  if (!have_lvd || !fsd_loc) return false;
  if (udf_block != k_block) return false;  // 4096-byte UDF: not supported (yet)

  // The logical volume's partition-relative addresses are added to the
  // partition start to get image blocks.
  const uint32_t fsd_block = part_start + fsd_loc;
  uint8_t fsd[k_block];
  if (!udf_read_block(iso, fsd_block, fsd)) return false;
  if (rd16(fsd) != 256) return false;  // file set descriptor
  const uint32_t root_icb = rd32(fsd + 400 + 4);

  iso->udf = true;
  iso->udf_block = udf_block;
  iso->udf_part_start = part_start;
  iso->root = udf_build(iso, nullptr, "/", part_start + root_icb, 0);
  if (!iso->root) return false;
  iso->root->is_dir = true;
  return true;
}

// ---- detection -------------------------------------------------------------
bool detect_sector_map(trt_iso* iso) {
  // cooked 2048 (ISO9660/UDF), raw 2352 (mode1: user data at +16),
  // 2336 (mode2: +8), 2352 mode2 form1 (+24).
  static const sector_map cands[] = {
      {2048, 0}, {2352, 16}, {2336, 8}, {2352, 24},
  };
  uint8_t buf[k_block];
  for (const sector_map& sm : cands) {
    const uint64_t off = 16ull * sm.stride + sm.user_off;
    if (off + k_block > iso->image_bytes) continue;
    if (!rd_at(iso, off, buf, k_block)) continue;
    if (memcmp(buf + 1, "CD001", 5) == 0) {
      iso->sm = sm;
      return true;
    }
    if (sm.stride == 2048) {
      // UDF: block 256 anchor, tag id 2
      uint8_t av[k_block];
      if (off + 256ull * k_block + k_block <= iso->image_bytes &&
          rd_at(iso, 256ull * k_block, av, k_block) && rd16(av) == 2) {
        iso->sm = sm;
        return true;
      }
    }
  }
  return false;
}

iso_node* lookup(const iso_node* root, const char* path) {
  if (!root) return nullptr;
  iso_node* cur = const_cast<iso_node*>(root);
  const char* p = path;
  while (*p == '/' || *p == '\\') ++p;
  while (*p) {
    const char* start = p;
    while (*p && *p != '/' && *p != '\\') ++p;
    std::string seg(start, (size_t)(p - start));
    if (seg.empty()) continue;
    if (seg == ".") continue;
    if (seg == "..") {
      if (cur->parent) cur = cur->parent;
      continue;
    }
    if (!cur->is_dir) return nullptr;
    iso_node* next = dir_find(cur, seg);
    if (!next) return nullptr;
    cur = next;
    while (*p == '/' || *p == '\\') ++p;
  }
  return cur;
}

}  // namespace

trt_iso* trt_iso_open(const char* host_path) {
  if (!host_path || !*host_path) return nullptr;
  trt_iso* iso = new trt_iso();
  iso->f = fopen(host_path, "rb");
  if (!iso->f) {
    delete iso;
    return nullptr;
  }
  const int64_t sz = iso_size(iso->f);
  if (sz <= 0) {
    fclose(iso->f);
    delete iso;
    return nullptr;
  }
  iso->image_bytes = (uint64_t)sz;
  if (!detect_sector_map(iso)) {
    fclose(iso->f);
    delete iso;
    return nullptr;
  }
  if (!open_iso9660(iso)) {
    // No ISO9660: a pure UDF disc.
    if (!open_udf(iso)) {
      fclose(iso->f);
      delete iso;
      return nullptr;
    }
  }
  return iso;
}

void trt_iso_close(trt_iso* iso) {
  if (!iso) return;
  if (iso->f) fclose(iso->f);
  node_free(iso->root);
  delete iso;
}

void trt_iso_get_info(const trt_iso* iso, struct trt_iso_info* out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  if (!iso) return;
  const char* fmt = "iso9660";
  if (iso->joliet) fmt = "joliet";
  if (iso->udf && iso->iso9660) fmt = "udf+iso";
  else if (iso->udf) fmt = "udf";
  snprintf(out->format, sizeof(out->format), "%s", fmt);
  out->sector_size = iso->sm.stride;
  out->user_offset = iso->sm.user_off;
  out->image_bytes = iso->image_bytes;
  out->file_count = iso->file_count;
  out->dir_count = iso->dir_count;
}

int trt_iso_stat(const trt_iso* iso, const char* path, int* is_dir,
                 uint64_t* size) {
  if (!iso || !iso->root) return -1;
  iso_node* n = lookup(iso->root, path ? path : "/");
  if (!n) return -1;
  if (is_dir) *is_dir = n->is_dir ? 1 : 0;
  if (size) *size = n->size;
  return 0;
}

int trt_iso_enum(const trt_iso* iso, const char* path, trt_iso_enum_cb cb,
                 void* user) {
  if (!iso || !iso->root) return -1;
  iso_node* n = lookup(iso->root, path ? path : "/");
  if (!n || !n->is_dir) return -1;
  for (iso_node* k : n->kids)
    if (cb) cb(user, k->name.c_str(), k->is_dir ? 1 : 0, k->size);
  return 0;
}

uint32_t trt_iso_read(const trt_iso* iso, const char* path, uint64_t off,
                      void* buf, uint32_t len) {
  if (!iso || !iso->root || !buf) return 0;
  iso_node* n = lookup(iso->root, path ? path : "/");
  if (!n || n->is_dir) return 0;
  uint8_t* dst = (uint8_t*)buf;
  uint32_t done = 0;
  uint64_t base = 0;  // image offset of the current extent's first byte
  for (const extent& e : n->ext) {
    const uint64_t ext_off = e.lba * iso->sm.stride + iso->sm.user_off;
    const uint64_t ext_end = base + e.len;
    if (off >= ext_end) {
      base = ext_end;
      continue;
    }
    uint64_t local = off - base;
    while (local < e.len && done < len) {
      // never read past the 2048-byte user area of a raw sector
      const uint64_t sector_index = local / k_block;
      const uint64_t in_sector = local % k_block;
      const uint64_t take = std::min<uint64_t>(
          std::min<uint64_t>(k_block - in_sector, e.len - local),
          (uint64_t)(len - done));
      if (take == 0) break;
      const uint64_t file_off =
          ext_off + sector_index * iso->sm.stride + in_sector;
      if (file_off + take > iso->image_bytes) break;
      if (!rd_at(const_cast<trt_iso*>(iso), file_off, dst + done,
                 (size_t)take))
        break;
      done += (uint32_t)take;
      local += take;
    }
    base = ext_end;
    if (done >= len) break;
  }
  return done;
}

int trt_iso_extract(const trt_iso* iso, const char* path, const char* host_out) {
  if (!iso || !host_out) return -1;
  int is_dir = 0;
  uint64_t size = 0;
  if (trt_iso_stat(iso, path, &is_dir, &size) != 0 || is_dir) return -1;
  FILE* out = fopen(host_out, "wb");
  if (!out) return -1;
  uint8_t buf[65536];
  uint64_t off = 0;
  while (off < size) {
    const uint32_t want =
        (uint32_t)std::min<uint64_t>(sizeof(buf), size - off);
    const uint32_t got = trt_iso_read(iso, path, off, buf, want);
    if (got == 0) break;
    if (fwrite(buf, 1, got, out) != got) {
      fclose(out);
      return -1;
    }
    off += got;
  }
  fclose(out);
  return off == size ? 0 : -1;
}

int trt_iso_extract_dir(const trt_iso* iso, const char* rel_dir,
                        const char* host_dir) {
  if (!iso || !iso->root || !host_dir) return 0;
  iso_node* dir = lookup(iso->root, rel_dir && *rel_dir ? rel_dir : "/");
  if (!dir || !dir->is_dir) return 0;
  int n = 0;
  for (iso_node* k : dir->kids) {
    if (k->is_dir) continue;
    char out[1600];
    snprintf(out, sizeof(out), "%s/%s", host_dir, k->name.c_str());
    for (char* q = out; *q; ++q)
      if (*q == '\\') *q = '/';
    const std::string rel =
        (rel_dir && *rel_dir) ? (std::string(rel_dir) + "/" + k->name)
                              : ("/" + k->name);
    if (trt_iso_extract(iso, rel.c_str(), out) == 0) ++n;
  }
  return n;
}

namespace {

const char k_host_sep =
#if defined(_WIN32)
    '\\';
#else
    '/';
#endif

bool starts_with_ci(const char* s, const char* prefix) {
  for (; *prefix; ++s, ++prefix) {
    char a = *s, b = *prefix;
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
    if (a != b) return false;
  }
  return true;
}

// A real mountable image, not just a file with the right extension (a truncated
// download or a .bin that is really an audio track must be skipped, not mounted).
bool image_mounts(const char* path) {
  trt_iso* iso = trt_iso_open(path);
  if (!iso) return false;
  trt_iso_close(iso);
  return true;
}

// A .cue sheet is a text index, not an image: the mountable file is the track
// its first FILE statement names.
int cue_track(const char* cue_path, char* out, size_t out_cap) {
  FILE* f = fopen(cue_path, "r");
  if (!f) return 0;
  char line[512];
  int found = 0;
  while (!found && fgets(line, sizeof(line), f)) {
    char* p = line;
    while (*p == ' ' || *p == '\t') ++p;
    if (!starts_with_ci(p, "FILE")) continue;
    p += 4;
    while (*p == ' ' || *p == '\t') ++p;
    char name[420] = {};
    if (*p == '"' || *p == '\'') {
      const char quote = *p++;
      const char* end = strchr(p, quote);
      if (!end) continue;
      const size_t n = (size_t)(end - p);
      if (n == 0 || n >= sizeof(name)) continue;
      memcpy(name, p, n);
      name[n] = 0;
    } else {
      size_t n = 0;
      while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
        if (n + 1 < sizeof(name)) name[n++] = *p;
        ++p;
      }
      name[n] = 0;
    }
    if (!name[0]) continue;
    // Absolute track path (C:\... or /...): use as-is. Otherwise relative to
    // the sheet's own folder, like every cue tool does.
    const bool absolute = name[0] == '/' || name[0] == '\\' ||
                          (name[1] == ':');
    if (absolute) {
      snprintf(out, out_cap, "%s", name);
    } else {
      const char* last = strrchr(cue_path, k_host_sep);
      size_t dir_len = last ? (size_t)(last - cue_path) : 0;
      if (dir_len >= out_cap) dir_len = out_cap - 1;
      memcpy(out, cue_path, dir_len);
      out[dir_len] = 0;
      snprintf(out + dir_len, out_cap - dir_len, "%c%s", k_host_sep, name);
    }
    found = 1;
  }
  fclose(f);
  return found;
}

bool ext_is(const char* ext, const char* want) {
  for (; *ext && *want; ++ext, ++want) {
    char a = *ext, b = *want;
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (a != b) return false;
  }
  return *ext == 0 && *want == 0;
}

// Rank an image candidate: a cooked .iso is preferred, then a .cue (whose track
// is the image), then raw .bin/.img dumps. 0 = not an image name.
int image_rank(const char* name) {
  const char* dot = strrchr(name, '.');
  if (!dot) return 0;
  if (ext_is(dot, ".iso")) return 4;
  if (ext_is(dot, ".cue")) return 3;
  if (ext_is(dot, ".bin")) return 2;
  if (ext_is(dot, ".img")) return 1;
  return 0;
}

// Depth-limited search: a game that keeps its disc in `disc/` or `game data/`
// is as common as one that keeps it beside the exe.
int find_image_rec(const char* dir, int depth, char* out, size_t out_cap,
                   int* best_rank) {
  if (depth > 2) return 0;
  std::vector<std::string> dirs;
  std::vector<std::string> names;
#if defined(_WIN32)
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(std::string(dir).append("\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return 0;
  do {
    if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) dirs.push_back(fd.cFileName);
    else names.push_back(fd.cFileName);
  } while (FindNextFileA(h, &fd));
  FindClose(h);
#else
  DIR* d = opendir(dir);
  if (!d) return 0;
  struct dirent* de;
  while ((de = readdir(d)) != nullptr) {
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
    struct stat st;
    std::string p = std::string(dir) + k_host_sep + de->d_name;
    if (stat(p.c_str(), &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) dirs.push_back(de->d_name);
    else names.push_back(de->d_name);
  }
  closedir(d);
#endif
  int found = 0;
  for (const auto& n : names) {
    const int rank = image_rank(n.c_str());
    if (rank <= *best_rank) continue;
    std::string p = std::string(dir) + k_host_sep + n;
    if (rank == 3) {
      char track[1200] = {};
      if (!cue_track(p.c_str(), track, sizeof(track))) continue;
      p = track;
    }
    if (!image_mounts(p.c_str())) continue;
    snprintf(out, out_cap, "%s", p.c_str());
    *best_rank = rank;
    found = 1;
  }
  for (const auto& d2 : dirs) {
    std::string p = std::string(dir) + k_host_sep + d2;
    if (find_image_rec(p.c_str(), depth + 1, out, out_cap, best_rank)) found = 1;
  }
  return found;
}

}  // namespace

int trt_iso_find_image_in_dir(const char* host_dir, char* out, size_t out_cap) {
  if (!host_dir || !out || out_cap < 8) return 0;
  int best_rank = 0;
  out[0] = 0;
  return find_image_rec(host_dir, 0, out, out_cap, &best_rank);
}

int trt_iso_cue_track(const char* cue_path, char* out_path, size_t out_cap) {
  if (!cue_path || !out_path || out_cap < 8) return 0;
  out_path[0] = 0;
  return cue_track(cue_path, out_path, out_cap);
}
