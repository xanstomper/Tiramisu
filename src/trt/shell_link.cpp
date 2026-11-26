// tiramisu/src/trt/shell_link.cpp  -- see shell_link.h (attribution there).
#include "shell_link.h"

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

namespace {

struct Reader {
  const u8* p;
  size_t n;
  template <class T>
  bool get(size_t off, T* v) const {
    if (off > n || sizeof(T) > n - off) return false;
    memcpy(v, p + off, sizeof(T));
    return true;
  }
};

// ANSI strings in a shortcut are in the creator's code page; the runtime has
// no code-page table here, so decode as UTF-8 and fall back to Latin-1.
std::u16string decode(const u8* s, size_t count, bool unicode) {
  std::u16string out;
  if (unicode) {
    for (size_t i = 0; i < count; ++i) {
      u16 c;
      memcpy(&c, s + i * 2, 2);
      out += (char16_t)c;
    }
    return out;
  }
  for (size_t i = 0; i < count;) {
    u8 c = s[i];
    if (c < 0x80) { out += (char16_t)c; ++i; }
    else if (c >= 0xC2 && c < 0xE0 && i + 1 < count && (s[i + 1] & 0xC0) == 0x80) {
      out += (char16_t)(((c & 0x1F) << 6) | (s[i + 1] & 0x3F)); i += 2;
    } else if (c >= 0xE0 && c < 0xF0 && i + 2 < count && (s[i + 1] & 0xC0) == 0x80 &&
               (s[i + 2] & 0xC0) == 0x80) {
      out += (char16_t)(((c & 0xF) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F)); i += 3;
    } else { out += (char16_t)c; ++i; }  // Latin-1
  }
  return out;
}

}  // namespace

bool trt_shelllink_parse(const u8* data, size_t len, trt_shelllink* out) {
  static const u8 kClsid[16] = {1, 0x14, 2, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46};
  constexpr size_t kHeader = 0x4C;
  if (!data || !out || len < kHeader || len > kTrtShellLinkMaxSize ||
      memcmp(data + 4, kClsid, sizeof(kClsid)) != 0)
    return false;
  Reader r{data, len};
  u32 header_size = 0, flags = 0;
  r.get(0, &header_size);
  if (header_size != kHeader) return false;
  r.get(20, &flags);
  trt_shelllink link;
  i32 icon_index = 0;
  r.get(56, &icon_index);
  link.icon_index = icon_index;
  r.get(60, &link.show_command);
  r.get(64, &link.hot_key);

  size_t cur = kHeader;
  if (flags & 0x1) {  // HasLinkTargetIDList
    u16 sz = 0;
    if (!r.get(cur, &sz) || sz > len - cur - 2) return false;
    link.has_idlist = true;
    cur += 2 + sz;
  }
  if (flags & 0x2) {  // HasLinkInfo
    u32 size = 0, hdr = 0, info_flags = 0;
    if (!r.get(cur, &size) || size < 0x1C || size > len - cur ||
        !r.get(cur + 4, &hdr) || hdr < 0x1C || hdr > size || (hdr > 0x1C && hdr < 0x24))
      return false;
    r.get(cur + 8, &info_flags);
    if ((info_flags & 1) && !(flags & 0x100)) {  // VolumeIDAndLocalBasePath
      auto path_at = [&](size_t ansi_field, size_t uni_field, std::u16string* v) {
        u32 off = 0;
        if (hdr >= 0x24) r.get(cur + uni_field, &off);
        const bool uni = off != 0;
        if (!uni) r.get(cur + ansi_field, &off);
        if (off < hdr || off >= size) return false;
        const size_t unit = uni ? 2 : 1;
        for (size_t e = off; unit <= size - e; e += unit) {
          const u8* q = data + cur + e;
          if (q[0] == 0 && (!uni || q[1] == 0)) {
            *v = decode(data + cur + off, (e - off) / unit, uni);
            return v->find(u'\0') == std::u16string::npos;
          }
        }
        return false;
      };
      std::u16string suffix;
      if (!path_at(16, 28, &link.target) || !path_at(24, 32, &suffix)) return false;
      link.target += suffix;
    }
    cur += size;
  }

  // StringData, fixed order: NAME, RELATIVE_PATH, WORKING_DIR, ARGUMENTS, ICON
  std::u16string* fields[] = {&link.description, &link.relative_path, &link.working_dir,
                              &link.arguments, &link.icon_path};
  const bool uni = (flags & 0x80) != 0;  // IsUnicode
  for (size_t i = 0; i < 5; ++i) {
    if (!(flags & (4u << i))) continue;
    u16 count = 0;
    if (!r.get(cur, &count)) return false;
    cur += 2;
    const size_t bytes = (size_t)count * (uni ? 2 : 1);
    if (bytes > len - cur) return false;
    *fields[i] = decode(data + cur, count, uni);
    if (fields[i]->find(u'\0') != std::u16string::npos) return false;
    cur += bytes;
  }
  *out = std::move(link);
  return true;
}

bool trt_shelllink_load(const u16* guest_path, trt_shelllink* out) {
  char host[1024];
  if (!guest_path || trt_vfs_resolve_utf16(guest_path, host, sizeof(host)) != 0) return false;
  FILE* f = fopen(host, "rb");
  if (!f) return false;
  std::vector<u8> buf;
  u8 tmp[4096];
  size_t got;
  while ((got = fread(tmp, 1, sizeof(tmp), f)) > 0) {
    buf.insert(buf.end(), tmp, tmp + got);
    if (buf.size() > kTrtShellLinkMaxSize) { fclose(f); return false; }
  }
  fclose(f);
  return trt_shelllink_parse(buf.data(), buf.size(), out);
}

std::u16string trt_shelllink_launch_target(const trt_shelllink& link,
                                           const std::u16string& link_path) {
  if (!link.target.empty()) return link.target;
  if (link.relative_path.empty()) return {};
  const std::u16string& rel = link.relative_path;
  const bool absolute = (rel.size() >= 2 && rel[1] == u':') || rel[0] == u'\\' || rel[0] == u'/';
  if (absolute) return rel;
  size_t s = link_path.find_last_of(u"\\/");
  std::u16string dir = s == std::u16string::npos ? std::u16string() : link_path.substr(0, s + 1);
  std::u16string r = rel;
  if (r.rfind(u".\\", 0) == 0 || r.rfind(u"./", 0) == 0) r = r.substr(2);
  return dir + r;
}
