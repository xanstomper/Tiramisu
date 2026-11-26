// tiramisu/tests/test_iso.cpp
// The disc-image gate: build a real ISO9660+Joliet image and a real UDF image
// in memory, mount each through the same API the VFS uses, then stat,
// enumerate, read and extract files back out byte-for-byte.
//
// This is the "any .iso is bootable" evidence: a disc game's data files are
// reachable through the guest's own path requests without extracting the
// whole image.
#include "../src/trt/iso.h"
#include "../src/trt/trt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const char* what) {
  if (ok) {
    ++g_pass;
    printf("  PASS %s\n", what);
  } else {
    ++g_fail;
    printf("  FAIL %s\n", what);
  }
}

// ---------------------------------------------------------------- image build
struct img {
  std::vector<unsigned char> b;
  void ensure(size_t n) {
    if (b.size() < n) b.resize(n, 0);
  }
  void put16(size_t off, unsigned v) {
    ensure(off + 2);
    b[off] = (unsigned char)v;
    b[off + 1] = (unsigned char)(v >> 8);
  }
  void put32(size_t off, unsigned v) {
    ensure(off + 4);
    for (int i = 0; i < 4; ++i) b[off + i] = (unsigned char)(v >> (8 * i));
  }
  void put32be(size_t off, unsigned v) {
    ensure(off + 4);
    for (int i = 0; i < 4; ++i) b[off + i] = (unsigned char)(v >> (8 * (3 - i)));
  }
  void put16be(size_t off, unsigned v) {
    ensure(off + 2);
    b[off] = (unsigned char)(v >> 8);
    b[off + 1] = (unsigned char)v;
  }
  void both32(size_t off, unsigned v) {
    put32(off, v);
    put32be(off + 4, v);
  }
  void both16(size_t off, unsigned v) {
    put16(off, v);
    put16be(off + 2, v);
  }
  void bytes(size_t off, const void* p, size_t n) {
    ensure(off + n);
    memcpy(b.data() + off, p, n);
  }
};

static const unsigned kSec = 2048;

// A directory record inside a directory extent.
static size_t dir_record(unsigned char* dst, unsigned lba, unsigned len,
                         int is_dir, const char* name, int joliet) {
  unsigned char* r = dst;
  size_t name_len;
  unsigned char nb[256];
  memset(nb, 0, sizeof(nb));
  if (!strcmp(name, ".") || !strcmp(name, "..")) {
    name_len = 1;
    nb[0] = name[0] == '.' && name[1] == 0 ? 0 : 1;
  } else if (joliet) {
    name_len = strlen(name) * 2;
    for (size_t i = 0; i < strlen(name); ++i) {
      nb[i * 2] = 0;
      nb[i * 2 + 1] = (unsigned char)name[i];
    }
  } else {
    name_len = strlen(name);
    memcpy(nb, name, name_len);
  }
  const size_t rec_len = 33 + name_len + (name_len & 1);
  r[0] = (unsigned char)rec_len;
  r[1] = 0;
  r[2] = (unsigned char)lba;
  r[3] = (unsigned char)(lba >> 8);
  r[4] = (unsigned char)(lba >> 16);
  r[5] = (unsigned char)(lba >> 24);
  r[6] = (unsigned char)(lba >> 24);
  r[7] = (unsigned char)(lba >> 16);
  r[8] = (unsigned char)(lba >> 8);
  r[9] = (unsigned char)lba;
  r[10] = (unsigned char)len;
  r[11] = (unsigned char)(len >> 8);
  r[12] = (unsigned char)(len >> 16);
  r[13] = (unsigned char)(len >> 24);
  r[14] = (unsigned char)(len >> 24);
  r[15] = (unsigned char)(len >> 16);
  r[16] = (unsigned char)(len >> 8);
  r[17] = (unsigned char)len;
  r[25] = is_dir ? 0x02 : 0x00;
  r[28] = 1;
  r[29] = 0;
  r[30] = 1;
  r[31] = 0;
  r[32] = (unsigned char)name_len;
  memcpy(r + 33, nb, name_len);
  return rec_len;
}

// Build an ISO9660 image with a Joliet tree:
//   /README.TXT           (plain iso root, 40 bytes)
//   /DATA/LEVEL1.PAK      (joliet: Data/Level1.pak, 9000 bytes across sectors)
static std::vector<unsigned char> build_iso9660() {
  img m;
  const unsigned kBlocks = 64;
  m.b.assign(kBlocks * kSec, 0);

  // --- file data: LEVEL1.PAK at LBA 40, 9000 bytes
  std::string pak;
  for (int i = 0; i < 9000; ++i) pak.push_back((char)(i * 7 + 3));
  m.bytes(40ull * kSec, pak.data(), pak.size());
  const char* readme = "tiramisu iso9660 gate: hello from a disc image\n";
  m.bytes(30ull * kSec, readme, strlen(readme));

  // --- PVD at LBA 16
  {
    unsigned char* vd = m.b.data() + 16 * kSec;
    vd[0] = 1;
    memcpy(vd + 1, "CD001", 5);
    vd[6] = 1;
    // volume space size (both-endian 8)
    m.both32(16 * kSec + 80, kBlocks);
    m.put16(16 * kSec + 120, 1);   // volume set size
    m.put16(16 * kSec + 124, 1);   // volume sequence number
    m.both16(16 * kSec + 128, kSec);
    // root directory record: LBA 20, 2048 bytes
    dir_record(vd + 156, 20, kSec, 1, ".", 0);
    vd[156] = 34;
  }
  // --- Joliet SVD at LBA 17
  {
    unsigned char* vd = m.b.data() + 17 * kSec;
    vd[0] = 2;
    memcpy(vd + 1, "CD001", 5);
    vd[6] = 1;
    vd[88] = '%';
    vd[89] = '/';
    vd[90] = 'E';
    m.both32(17 * kSec + 80, kBlocks);
    m.put16(17 * kSec + 120, 1);
    m.put16(17 * kSec + 124, 1);
    m.both16(17 * kSec + 128, kSec);
    dir_record(vd + 156, 21, kSec, 1, ".", 1);
    vd[156] = 34;
  }
  // --- terminator at LBA 18
  {
    unsigned char* vd = m.b.data() + 18 * kSec;
    vd[0] = 255;
    memcpy(vd + 1, "CD001", 5);
    vd[6] = 1;
  }

  // --- plain ISO root directory at LBA 20: ".", "..", README.TXT;1, DATA
  {
    unsigned char* d = m.b.data() + 20 * kSec;
    size_t o = 0;
    o += dir_record(d + o, 20, kSec, 1, ".", 0);
    o += dir_record(d + o, 20, kSec, 1, "..", 0);
    o += dir_record(d + o, 30, 44, 0, "README.TXT;1", 0);
    o += dir_record(d + o, 22, kSec, 1, "DATA", 0);
  }
  // --- plain ISO /DATA at LBA 22
  {
    unsigned char* d = m.b.data() + 22 * kSec;
    size_t o = 0;
    o += dir_record(d + o, 22, kSec, 1, ".", 0);
    o += dir_record(d + o, 20, kSec, 1, "..", 0);
    o += dir_record(d + o, 40, 9000, 0, "LEVEL1.PAK;1", 0);
  }
  // --- Joliet root at LBA 21: "Data" + "readme.txt"
  {
    unsigned char* d = m.b.data() + 21 * kSec;
    size_t o = 0;
    o += dir_record(d + o, 21, kSec, 1, ".", 1);
    o += dir_record(d + o, 21, kSec, 1, "..", 1);
    o += dir_record(d + o, 30, 44, 0, "readme.txt", 1);
    o += dir_record(d + o, 23, kSec, 1, "Data", 1);
  }
  // --- Joliet /Data at LBA 23
  {
    unsigned char* d = m.b.data() + 23 * kSec;
    size_t o = 0;
    o += dir_record(d + o, 23, kSec, 1, ".", 1);
    o += dir_record(d + o, 21, kSec, 1, "..", 1);
    o += dir_record(d + o, 40, 9000, 0, "Level1.pak", 1);
  }
  return m.b;
}

// ---------------------------------------------------------------- UDF build
// Minimal but real UDF 1.02: AVDP -> VDS(PVD, PD, LVD, TD) -> FSD -> root FE
// with one directory and one file, all in 2048-byte blocks.
static void udf_tag(unsigned char* p, unsigned id, unsigned loc) {
  memset(p, 0, 16);
  p[0] = (unsigned char)id;
  p[1] = (unsigned char)(id >> 8);
  p[2] = 2;  // version
  p[4] = (unsigned char)loc;
  p[5] = (unsigned char)(loc >> 8);
  p[6] = (unsigned char)(loc >> 16);
  p[7] = (unsigned char)(loc >> 24);
}

static void udf_long_ad(unsigned char* p, unsigned len, unsigned loc,
                        unsigned part) {
  memset(p, 0, 16);
  p[0] = (unsigned char)len;
  p[1] = (unsigned char)(len >> 8);
  p[2] = (unsigned char)(len >> 16);
  p[3] = (unsigned char)(len >> 24);
  p[4] = (unsigned char)loc;
  p[5] = (unsigned char)(loc >> 8);
  p[6] = (unsigned char)(loc >> 16);
  p[7] = (unsigned char)(loc >> 24);
  p[8] = (unsigned char)part;
  p[9] = (unsigned char)(part >> 8);
}

static void udf_put64(unsigned char* p, unsigned long long v) {
  for (int i = 0; i < 8; ++i) p[i] = (unsigned char)(v >> (8 * (7 - i)));
}

// A file entry (tag 261) for `is_dir` at block `loc`, with one extent.
static void udf_file_entry(unsigned char* fe, unsigned loc, bool is_dir,
                           unsigned long long size, unsigned data_loc,
                           unsigned part) {
  memset(fe, 0, kSec);
  udf_tag(fe, 261, loc);
  fe[16] = 0;                 // prior recorded entries
  fe[18] = 0;                 // strategy type
  fe[27] = is_dir ? 4 : 5;    // file type
  fe[28] = (unsigned char)(loc);       // parent ICB (irrelevant for the test)
  fe[34] = 0;
  fe[35] = 1;                 // flags: allocation descriptors are long_ad
  udf_put64(fe + 56, size);   // information length
  udf_put64(fe + 64, (size + kSec - 1) / kSec);
  // length of allocation descriptors at 172, descriptors start at 176
  fe[172] = 16;
  udf_long_ad(fe + 176, (unsigned)size, data_loc, part);
}

static std::vector<unsigned char> build_udf() {
  img m;
  const unsigned kBlocks = 400;
  m.b.assign(kBlocks * kSec, 0);

  const unsigned part_start = 100;  // partition begins at block 100
  const unsigned part_len = 250;

  // --- file data at partition block 20 (image block 120)
  const char* payload = "udf gate: this file lives inside a UDF volume\n";
  const unsigned payload_len = (unsigned)strlen(payload);
  m.bytes((part_start + 20ull) * kSec, payload, payload_len);

  // --- AVDP at block 256
  {
    unsigned char* p = m.b.data() + 256 * kSec;
    udf_tag(p, 2, 256);
    // main VDS extent_ad: length, location
    m.put32(256 * kSec + 16, 3 * kSec);
    m.put32(256 * kSec + 20, 32);
  }
  // --- VDS at blocks 32..34
  {
    // PVD
    unsigned char* pvd = m.b.data() + 32 * kSec;
    udf_tag(pvd, 1, 32);
    // PD at 33
    unsigned char* pd = m.b.data() + 33 * kSec;
    udf_tag(pd, 5, 33);
    m.put16(33 * kSec + 22, 0);  // partition number
    m.put32(33 * kSec + 188, part_start);
    m.put32(33 * kSec + 192, part_len);
    // LVD at 34
    unsigned char* lvd = m.b.data() + 34 * kSec;
    udf_tag(lvd, 6, 34);
    m.put32(34 * kSec + 212, kSec);           // logical block size
    // file set descriptor long_ad at 248: len, loc, part
    udf_long_ad(lvd + 248, kSec, 2, 0);       // FSD at partition block 2
    // TD at 35
    udf_tag(m.b.data() + 35 * kSec, 8, 35);
  }
  // --- FSD at partition block 2 (image block 102)
  {
    unsigned char* fsd = m.b.data() + (part_start + 2ull) * kSec;
    udf_tag(fsd, 256, part_start + 2);
    // root directory ICB: long_ad at offset 400 -> partition block 3
    udf_long_ad(fsd + 400, kSec, 3, 0);
  }
  // --- root FE at partition block 3 (image block 103), data at block 4
  udf_file_entry(m.b.data() + (part_start + 3ull) * kSec, part_start + 3,
                 true, 3 * kSec, part_start + 4, 0);
  // --- root directory data at partition block 4
  {
    unsigned char* d = m.b.data() + (part_start + 4ull) * kSec;
    // parent FID
    udf_tag(d, 257, 0);
    d[18] = 0x08;  // parent
    d[19] = 0;
    udf_long_ad(d + 20, kSec, part_start + 3, 0);
    unsigned off = 40;
    // "hello.txt" -> ICB at partition block 10
    {
      unsigned char* f = d + off;
      udf_tag(f, 257, 0);
      f[18] = 0x00;
      f[19] = 10;  // 1+9 bytes of CS0 name
      udf_long_ad(f + 20, kSec, part_start + 10, 0);
      m.put16(off + 36, 0);  // implementation use length
      f[38] = 8;             // CS0 compression id
      memcpy(f + 39, "hello.txt", 9);
      off += (38 + 0 + 10 + 3) & ~3u;
    }
    // "media" directory -> ICB at partition block 11
    {
      unsigned char* f = d + off;
      udf_tag(f, 257, 0);
      f[18] = 0x02;  // directory
      f[19] = 6;
      udf_long_ad(f + 20, kSec, part_start + 11, 0);
      m.put16(off + 36, 0);
      f[38] = 8;
      memcpy(f + 39, "media", 5);
      off += (38 + 0 + 6 + 3) & ~3u;
    }
  }
  // --- hello.txt FE at partition block 10 -> data at block 20
  udf_file_entry(m.b.data() + (part_start + 10ull) * kSec, part_start + 10,
                 false, payload_len, part_start + 20, 0);
  // --- media dir FE at block 11, data at 12 with one file "movie.dat"
  udf_file_entry(m.b.data() + (part_start + 11ull) * kSec, part_start + 11,
                 true, kSec, part_start + 12, 0);
  {
    unsigned char* d = m.b.data() + (part_start + 12ull) * kSec;
    udf_tag(d, 257, 0);
    d[18] = 0x08;
    d[19] = 0;
    udf_long_ad(d + 20, kSec, part_start + 11, 0);
    unsigned char* f = d + 40;
    udf_tag(f, 257, 0);
    f[18] = 0x00;
    f[19] = 10;
    udf_long_ad(f + 20, kSec, part_start + 13, 0);
    m.put16(40 + 36, 0);
    f[38] = 8;
    memcpy(f + 39, "movie.dat", 9);
  }
  const char* movie = "media payload";
  m.bytes((part_start + 14ull) * kSec, movie, strlen(movie));
  udf_file_entry(m.b.data() + (part_start + 13ull) * kSec, part_start + 13,
                 false, (unsigned long long)strlen(movie), part_start + 14, 0);

  return m.b;
}

// ---------------------------------------------------------------------- tests
struct enum_rec {
  std::vector<std::string> names;
  std::vector<std::string> dirs;
};

static void enum_cb(void* user, const char* name, int is_dir, uint64_t size) {
  enum_rec* r = (enum_rec*)user;
  r->names.push_back(name);
  if (is_dir) r->dirs.push_back(name);
  (void)size;
}

static void write_file(const char* path, const std::vector<unsigned char>& b) {
  FILE* f = fopen(path, "wb");
  if (!f) return;
  fwrite(b.data(), 1, b.size(), f);
  fclose(f);
}

static void test_iso9660_joliet() {
  printf("STAGE 1 ISO9660 + Joliet\n");
  const char* path = "/tmp/opencode/tiramisu-test.iso";
  write_file(path, build_iso9660());
  trt_iso* iso = trt_iso_open(path);
  check(iso != nullptr, "mounts a real ISO9660 image");
  if (!iso) return;
  trt_iso_info info;
  trt_iso_get_info(iso, &info);
  check(!strcmp(info.format, "joliet"), "Joliet volume descriptor preferred");
  check(info.sector_size == 2048, "cooked 2048-byte sectors");

  int is_dir = -1;
  uint64_t size = 0;
  check(trt_iso_stat(iso, "/", &is_dir, &size) == 0 && is_dir == 1,
        "root is a directory");
  check(trt_iso_stat(iso, "/Data", &is_dir, &size) == 0 && is_dir == 1,
        "Joliet directory name is case-insensitive");
  check(trt_iso_stat(iso, "\\DATA\\LEVEL1.PAK", &is_dir, &size) == 0 &&
            is_dir == 0 && size == 9000,
        "backslashes + case fold resolve to the file, real size");

  std::vector<unsigned char> got(9000);
  const unsigned n = trt_iso_read(iso, "/Data/Level1.pak", 0, got.data(), 9000);
  check(n == 9000, "reads the whole file");
  bool bytes_ok = (n == 9000);
  for (unsigned i = 0; bytes_ok && i < n; ++i)
    if (got[i] != (unsigned char)(i * 7 + 3)) bytes_ok = false;
  check(bytes_ok, "every byte matches the image");

  // partial read across a sector boundary
  std::vector<unsigned char> part(200);
  const unsigned pn = trt_iso_read(iso, "/Data/Level1.pak", 2000, part.data(), 200);
  check(pn == 200 && part[0] == (unsigned char)(2000 * 7 + 3),
        "offset read lands on the right byte");

  enum_rec er;
  check(trt_iso_enum(iso, "/Data", enum_cb, &er) == 0, "enumerates a directory");
  check(er.names.size() == 1 && er.names[0] == "Level1.pak",
        "enumeration returns the real name (no ;1, no padding)");

  enum_rec root;
  trt_iso_enum(iso, "/", enum_cb, &root);
  check(root.names.size() == 2 && root.dirs.size() == 1,
        "root lists 1 file + 1 directory");

  const char* out = "/tmp/opencode/tiramisu-iso-extract.pak";
  remove(out);
  check(trt_iso_extract(iso, "/Data/Level1.pak", out) == 0, "extracts to cache");
  FILE* f = fopen(out, "rb");
  long fsz = 0;
  if (f) {
    fseek(f, 0, SEEK_END);
    fsz = ftell(f);
    fclose(f);
  }
  check(fsz == 9000, "extracted file is the full size");
  remove(out);
  trt_iso_close(iso);
}

static void test_udf() {
  printf("STAGE 2 UDF 1.02\n");
  const char* path = "/tmp/opencode/tiramisu-test.udf.iso";
  write_file(path, build_udf());
  trt_iso* iso = trt_iso_open(path);
  check(iso != nullptr, "mounts a real UDF image");
  if (!iso) return;
  trt_iso_info info;
  trt_iso_get_info(iso, &info);
  check(!strcmp(info.format, "udf"), "detected as UDF");

  int is_dir = -1;
  uint64_t size = 0;
  const char* want = "udf gate: this file lives inside a UDF volume\n";
  const uint64_t want_len = (uint64_t)strlen(want);
  check(trt_iso_stat(iso, "/hello.txt", &is_dir, &size) == 0 && is_dir == 0 &&
            size == want_len,
        "root file stat (name + size)");
  char buf[64] = {};
  const unsigned n = trt_iso_read(iso, "/hello.txt", 0, buf, (uint32_t)want_len);
  check(n == want_len && !memcmp(buf, want, want_len),
        "reads the UDF file bytes");

  enum_rec er;
  check(trt_iso_enum(iso, "/", enum_cb, &er) == 0 && er.names.size() == 2,
        "UDF root enumerates 2 entries");
  check(er.dirs.size() == 1 && er.dirs[0] == "media", "subdirectory listed");
  check(trt_iso_stat(iso, "/media/movie.dat", &is_dir, &size) == 0 &&
            size == 13,
        "nested UDF path resolves");

  trt_iso_close(iso);
}

static void test_raw_2352() {
  printf("STAGE 3 raw 2352 sector dump (.bin)\n");
  // Wrap the cooked image in 2352-byte raw mode-1 sectors.
  std::vector<unsigned char> cooked = build_iso9660();
  std::vector<unsigned char> raw;
  const size_t sectors = cooked.size() / kSec;
  raw.resize(sectors * 2352, 0);
  for (size_t s = 0; s < sectors; ++s) {
    unsigned char* sec = raw.data() + s * 2352;
    sec[0] = 0x00;  // sync
    for (int i = 1; i < 11; ++i) sec[i] = 0xFF;
    sec[11] = 0x00;
    sec[15] = 0x01;  // mode 1
    memcpy(sec + 16, cooked.data() + s * kSec, kSec);
  }
  const char* path = "/tmp/opencode/tiramisu-test.bin";
  write_file(path, raw);
  trt_iso* iso = trt_iso_open(path);
  check(iso != nullptr, "detects raw 2352 mode-1 sectors");
  if (!iso) return;
  trt_iso_info info;
  trt_iso_get_info(iso, &info);
  check(info.sector_size == 2352 && info.user_offset == 16,
        "sector map is 2352/+16");
  int is_dir = 0;
  uint64_t size = 0;
  check(trt_iso_stat(iso, "/Data/Level1.pak", &is_dir, &size) == 0 && size == 9000,
        "file resolves through the raw dump");
  std::vector<unsigned char> got(9000);
  check(trt_iso_read(iso, "/Data/Level1.pak", 0, got.data(), 9000) == 9000,
        "reads across raw sectors");
  bool ok = true;
  for (unsigned i = 0; i < 9000; ++i)
    if (got[i] != (unsigned char)(i * 7 + 3)) ok = false;
  check(ok, "raw-dump bytes match");
  trt_iso_close(iso);
}

static void test_not_an_image() {
  printf("STAGE 4 negative: a file that is not an image\n");
  std::vector<unsigned char> junk(200000, 0x5A);
  write_file("/tmp/opencode/tiramisu-not-an-image.iso", junk);
  trt_iso* iso = trt_iso_open("/tmp/opencode/tiramisu-not-an-image.iso");
  check(iso == nullptr, "refuses a non-image (no silent empty drive)");
}

// ---- STAGE 5: the guest's own path requests hit the disc ------------------
// Mounted as D:, a disc game's files must answer through the exact APIs a game
// uses: CreateFileW, FindFirstFileExW, GetFileAttributesExW, ReadFile.
extern "C" {
void* TRT_STDCALL trt_CreateFileW(const u16*, u32, u32, void*, u32, u32, void*);
int TRT_STDCALL trt_ReadFile(void*, void*, u32, u32*, void*);
int TRT_STDCALL trt_CloseHandle(void*);
void* TRT_STDCALL trt_FindFirstFileExW(const u16*, u32, void*, u32, void*, u32);
int TRT_STDCALL trt_FindNextFileW(void*, void*);
int TRT_STDCALL trt_FindClose(void*);
int TRT_STDCALL trt_GetFileAttributesExW(const u16*, u32, void*);
u32 TRT_STDCALL trt_GetFileAttributesW(const u16*);
int TRT_STDCALL trt_DeleteFileW(const u16*);
u32 TRT_STDCALL trt_GetLastError(void);
}

static void ascii_to_u16(const char* in, u16* out, size_t cap) {
  size_t i = 0;
  for (; in[i] && i + 1 < cap; ++i) out[i] = (u16)(unsigned char)in[i];
  out[i] = 0;
}

static void test_guest_apis() {
  printf("STAGE 5 guest path requests on a mounted disc\n");
  const char* img = "/tmp/opencode/tiramisu-test.iso";
  write_file(img, build_iso9660());

  trt_vfs_set_iso_cache_root("/tmp/opencode/tiramisu-iso-cache");
  check(trt_vfs_mount_image("d:", img) == 1, "image mounts as a drive");
  u16 wpath[512];
  ascii_to_u16("D:\\Data\\Level1.pak", wpath, 512);
  check(trt_vfs_is_image_utf16(wpath) == 1,
        "D: paths are routed to the image");

  // GetFileAttributesExW: real size, no extraction
  u8 fad[36] = {};
  check(trt_GetFileAttributesExW(wpath, 0, fad) == 1,
        "GetFileAttributesExW answers for a disc file");
  const u32 sz_lo = *(u32*)(fad + 32);
  check(sz_lo == 9000, "reported size is the file's real size");
  check(*(u32*)(fad + 0) == 0x80u, "attributes are FILE_ATTRIBUTE_NORMAL");
  check(trt_GetFileAttributesW(wpath) == 0x80u, "GetFileAttributesW agrees");

  // CreateFileW + ReadFile: the actual bytes
  void* h = trt_CreateFileW(wpath, 0x80000000u, 1u, nullptr, 3u, 0x80u, nullptr);
  check(h != (void*)(uintptr_t)-1, "CreateFileW opens the disc file");
  if (h != (void*)(uintptr_t)-1) {
    std::vector<unsigned char> buf(9000);
    u32 got = 0;
    check(trt_ReadFile(h, buf.data(), 9000, &got, nullptr) == 1 && got == 9000,
          "ReadFile returns the whole file");
    bool ok = (got == 9000);
    for (u32 i = 0; ok && i < got; ++i)
      if (buf[i] != (unsigned char)(i * 7 + 3)) ok = false;
    check(ok, "bytes read from the disc match the image");
    trt_CloseHandle(h);
  }

  // FindFirstFileExW over a disc directory
  u16 wdir[512];
  ascii_to_u16("D:\\Data\\*", wdir, 512);
  u8 fd[592] = {};
  void* fh = trt_FindFirstFileExW(wdir, 0, fd, 0, nullptr, 0);
  check(fh != (void*)(uintptr_t)-1, "FindFirstFileExW lists a disc directory");
  if (fh != (void*)(uintptr_t)-1) {
    const u16* name = (const u16*)(fd + 44);
    bool name_ok = name[0] == 'L' && name[1] == 'e' && name[2] == 'v';
    check(name_ok, "the real file name comes back");
    check(*(u32*)(fd + 32) == 9000, "the entry carries the real size");
    check(trt_FindNextFileW(fh, fd) == 0, "no phantom second entry");
    trt_FindClose(fh);
  }

  // read-only: a disc refuses writes, visibly
  void* hw = trt_CreateFileW(wpath, 0x40000000u, 0u, nullptr, 2u, 0x80u, nullptr);
  check(hw == (void*)(uintptr_t)-1, "write open is refused");
  check(trt_GetLastError() == 19, "the refusal is ERROR_WRITE_PROTECT");
  check(trt_DeleteFileW(wpath) == 0 && trt_GetLastError() == 19,
        "DeleteFileW on a disc answers WRITE_PROTECT");

  trt_vfs_unmount_image("d:");
  check(trt_vfs_is_image_utf16(wpath) == 0, "unmounting removes the drive");
}

// Discovery: a game that keeps its disc in a subfolder, and a .cue sheet whose
// FILE track is the mountable image, must both be found and mounted - the two
// layouts where "the game files were not mounted" used to be the whole bug.
static void test_image_discovery() {
  printf("STAGE 6 image discovery (.cue, nested folders)\n");
  const char* root = "/tmp/opencode/tira-discover";
  const char* nested = "/tmp/opencode/tira-discover/game/disc";
  char cmd[512];
  snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", root, nested);
  if (system(cmd) != 0) check(false, "fixture directories created");

  std::string iso_rel = std::string(nested) + "/payload.iso";
  write_file(iso_rel.c_str(), build_iso9660());

  char found[1200] = {};
  check(trt_iso_find_image_in_dir(root, found, sizeof(found)) == 1,
        "finds an image two directories down");
  check(strcmp(found, iso_rel.c_str()) == 0, "returns the nested image path");

  // The same image referenced by a .cue sheet: the sheet is not mountable, its
  // FILE track is.
  const char* sheet = "/tmp/opencode/tira-discover/game/disc/game.cue";
  FILE* f = fopen(sheet, "w");
  if (f) {
    fprintf(f, "FILE \"payload.iso\" BINARY\n  TRACK 01 MODE1/2048\n");
    fclose(f);
  }
  char track[1200] = {};
  check(trt_iso_cue_track(sheet, track, sizeof(track)) == 1,
        "a .cue resolves its FILE track");
  check(strcmp(track, iso_rel.c_str()) == 0,
        "the resolved track is the sheet's own directory + FILE name");

  // A folder whose only disc artefact is a .cue naming a track that is not a
  // real image must report nothing rather than mounting garbage.
  const char* junk = "/tmp/opencode/tira-discover/junk";
  snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", junk, junk);
  if (system(cmd) != 0) check(false, "junk fixture created");
  FILE* g = fopen((std::string(junk) + "/data.bin").c_str(), "wb");
  if (g) {
    const char text[] = "not a filesystem";
    fwrite(text, 1, sizeof(text) - 1, g);
    fclose(g);
  }
  FILE* h = fopen((std::string(junk) + "/game.cue").c_str(), "w");
  if (h) {
    fprintf(h, "FILE \"data.bin\" BINARY\n");
    fclose(h);
  }
  char none[1200] = "untouched";
  check(trt_iso_find_image_in_dir(junk, none, sizeof(none)) == 0,
        "a .cue whose track is not an image is not mounted");
}

int main() {
  printf("== tiramisu disc-image gate ==\n");
  test_iso9660_joliet();
  test_udf();
  test_raw_2352();
  test_not_an_image();
  test_guest_apis();
  test_image_discovery();
  printf("%s (%d pass, %d fail)\n", g_fail ? "RESULT: FAIL" : "RESULT: ALL PASS",
         g_pass, g_fail);
  return g_fail ? 1 : 0;
}
