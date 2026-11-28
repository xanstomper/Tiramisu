// GDI glyph ABI and packing checks. The Windows run also exercises DirectWrite.
#include "../src/trt/gdi_glyph.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

int failures = 0;
void check(bool pass, const char* message) {
  printf("%s %s\n", pass ? "PASS" : "FAIL", message);
  if (!pass) ++failures;
}

#ifdef _WIN32
// Validate TTPOLYGONHEADER and the variable-size records inside each contour.
bool native_contours(const std::vector<u8>& bytes) {
  if (bytes.empty()) return false;
  for (size_t i = 0; i < bytes.size();) {
    if (bytes.size() - i < 16) return false;
    u32 cb, type;
    memcpy(&cb, bytes.data() + i, sizeof(cb));
    memcpy(&type, bytes.data() + i + 4, sizeof(type));
    if (type != 24 || cb < 16 || cb > bytes.size() - i) return false;
    const size_t end = i + cb;
    i += 16;
    while (i < end) {
      if (end - i < 4) return false;
      u16 curve, count;
      memcpy(&curve, bytes.data() + i, sizeof(curve));
      memcpy(&count, bytes.data() + i + 2, sizeof(count));
      if ((curve != 1 && curve != 2) || count < (curve == 2 ? 2 : 1))
        return false;
      i += 4 + 8 * size_t(count);
    }
    if (i != end) return false;
  }
  return true;
}

void test_native_face() {
  using namespace trt_glyph;
  const matrix identity{{0,1},{0,0},{0,0},{0,1}};
  LOGFONTW font{};
  font.lfHeight = -28;
  memcpy(font.lfFaceName, L"Arial", sizeof(L"Arial"));
  auto load = [&](u32 character, u32 kind, glyph_metrics& gm) {
    const auto size = outline(&font, character, kind, &gm, 0, nullptr, &identity);
    if (!size || size == error || size > 1024 * 1024) return std::vector<u8>{};
    std::vector<u8> data(size, 0xCC);
    const auto read = outline(&font, character, kind, &gm, size, data.data(), &identity);
    if (read != size) data.clear();
    return data;
  };
  glyph_metrics a{}, b{};
  const auto outline_a = load('A', native, a);
  const auto outline_b = load('B', native, b);
  check(native_contours(outline_a) && native_contours(outline_b) &&
            outline_a != outline_b && a.width && a.height && a.advance_x > 0,
        "Distinct characters produce valid native contours and metrics");
  const auto mono = load('A', bitmap, a);
  check(!mono.empty() && std::any_of(mono.begin(), mono.end(), [](u8 p){return p != 0;}),
        "The selected face produces a monochrome glyph bitmap");
  const auto gray = load('B', gray8, b);
  check(!gray.empty() && std::any_of(gray.begin(), gray.end(), [](u8 p){return p != 0;}) &&
            *std::max_element(gray.begin(), gray.end()) <= 64,
        "The selected face produces GDI 0–64 grayscale coverage");
  const u16 text[] = {'A', 'B', 0x0378};
  u16 glyphs[3]{};
  check(indices(&font, text, 3, glyphs, true) == 3 &&
            glyphs[0] && glyphs[1] && glyphs[0] != glyphs[1] && glyphs[2] == 0xffff,
        "Glyph mapping uses the selected font and marks missing characters");
  check(load(glyphs[0], native | glyph_index, a) == outline_a,
        "The native glyph index selects the same outline as the character");
  glyph_metrics space{};
  check(outline(&font, ' ', native, &space, 0, nullptr, &identity) == 0 &&
            space.advance_x > 0, "Whitespace has an advance and no outline");
  u8 sentinel = 0x7a;
  check(outline(&font, 'W', native, &a, 1, &sentinel, &identity) == error &&
            sentinel == 0x7a, "An undersized glyph buffer remains untouched");
}
#endif

}  // namespace

int main() {
  using namespace trt_glyph;
  const u8 samples[] = {0, 64, 128, 255, 255, 0, 128, 64};
  u8 dest[9];
  memset(dest, 0xff, sizeof(dest));
  check(pack_bitmap(gray8, 3, 2, nullptr, 0, 0, nullptr) == 8,
        "Gray rows are DWORD-aligned in a size query");
  check(pack_bitmap(gray8, 3, 2, samples, 4, sizeof(dest), dest) == 8 &&
            dest[0] == 0 && dest[1] == 16 && dest[2] == 32 && dest[3] == 0 &&
            dest[4] == 64 && dest[5] == 0 && dest[6] == 32 && dest[7] == 0 &&
            dest[8] == 255, "Gray rows scale coverage and clear only their padding");
  check(pack_bitmap(bitmap, 3, 2, samples, 4, sizeof(dest), dest) == 8 &&
            dest[0] == 0x20 && dest[4] == 0xa0 && dest[1] == 0 && dest[7] == 0,
        "Monochrome pixels are packed most-significant-bit first");
  memset(dest, 0xbc, sizeof(dest));
  check(pack_bitmap(gray4, 3, 2, samples, 4, 7, dest) == error &&
            std::all_of(dest, dest + sizeof(dest), [](u8 p){return p == 0xbc;}),
        "Insufficient capacity leaves the caller's pixels untouched");
  check(pack_bitmap(gray2, 0xffffffffu, 10, nullptr, 0, 0, nullptr) == error &&
            pack_bitmap(gray2, 0, 10, nullptr, 0, 0, nullptr) == 0,
        "Coverage size arithmetic rejects overflow and handles blank glyphs");
#ifdef _WIN32
  test_native_face();
#endif
  return failures != 0;
}
