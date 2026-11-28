#include "gdi_glyph.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#pragma comment(lib, "dwrite.lib")
#endif

namespace trt_glyph {

u32 pack_bitmap(u32 kind, u32 width, u32 height, const u8* alpha,
                u32 alpha_stride, u32 capacity, u8* dest) {
  const bool mono = kind == bitmap;
  const u32 levels = kind == gray2 ? 4 : kind == gray4 ? 16 : kind == gray8 ? 64 : 0;
  if (!mono && !levels) return error;
  if (!width || !height) return 0;
  const u64 stride = mono ? (u64(width) + 31u) / 32u * 4u
                          : (u64(width) + 3u) & ~u64(3);
  if (stride > (error - 1) / height) return error;
  const u32 size = static_cast<u32>(stride * height);
  if (!dest || !capacity) return size;
  if (!alpha || alpha_stride < width || capacity < size) return error;

  memset(dest, 0, size);
  for (u32 y = 0; y < height; ++y) {
    const u8* src = alpha + size_t(y) * alpha_stride;
    u8* row = dest + size_t(y) * stride;
    for (u32 x = 0; x < width; ++x) {
      if (mono) {
        if (src[x] >= 128) row[x / 8] |= u8(0x80u >> (x % 8));
      } else {
        row[x] = static_cast<u8>((src[x] * levels + 127u) / 255u);
      }
    }
  }
  return size;
}

#ifdef _WIN32
namespace {

using Microsoft::WRL::ComPtr;
constexpr u32 max_bitmap_pixels = 4 * 1024 * 1024;

struct point {
  float x, y;
};

// The two APIs use opposite Y axes. The matrix supplied by GDI transforms
// around the glyph origin in its own coordinates.
float fixed_value(fixed f) { return f.integer + f.fraction / 65536.f; }

DWRITE_MATRIX transform(const matrix& m) {
  return {fixed_value(m.m11), -fixed_value(m.m12),
          -fixed_value(m.m21), fixed_value(m.m22), 0, 0};
}

point apply(const DWRITE_MATRIX& m, point p) {
  return {m.m11 * p.x + m.m21 * p.y, m.m12 * p.x + m.m22 * p.y};
}

// Windows' contour record consists of a TTPOLYGONHEADER followed by packed
// TTPOLYCURVE records. POINTFX is two little-endian 16.16 fixed values.
struct pointfx { i32 x, y; };
struct polygon_header { u32 bytes, type; pointfx start; };
struct curve_header { u16 type, count; };
static_assert(sizeof(polygon_header) == 16 && sizeof(curve_header) == 4);

pointfx to_fixed(point p) {
  constexpr float limit = 32767.f;
  return {static_cast<i32>(std::round(std::clamp(p.x, -limit, limit) * 65536.f)),
          static_cast<i32>(std::round(std::clamp(-p.y, -limit, limit) * 65536.f))};
}

point midpoint(point a, point b) { return {(a.x + b.x) / 2, (a.y + b.y) / 2}; }

// DirectWrite exposes cubic segments, including TrueType quadratics converted
// to cubics. Recover quadratics for GGO_NATIVE, subdividing genuine cubics
// until the difference between the two inferred control points is subpixel.
class outline_sink final : public IDWriteGeometrySink {
 public:
  outline_sink(DWRITE_MATRIX matrix, bool cubic) : matrix_(matrix), cubic_(cubic) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) noexcept override {
    if (!result) return E_POINTER;
    *result = nullptr;
    if (iid != __uuidof(IUnknown) && iid != __uuidof(IDWriteGeometrySink))
      return E_NOINTERFACE;
    *result = this;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() noexcept override { return --references_; }
  void STDMETHODCALLTYPE SetFillMode(D2D1_FILL_MODE) noexcept override {}
  void STDMETHODCALLTYPE SetSegmentFlags(D2D1_PATH_SEGMENT) noexcept override {}

  void STDMETHODCALLTYPE BeginFigure(D2D1_POINT_2F start, D2D1_FIGURE_BEGIN) noexcept override {
    current_ = map(start);
    start_ = current_;
    polygon_ = data_.size();
    append(polygon_header{0, 24, to_fixed(start_)});
  }

  void STDMETHODCALLTYPE AddLines(const D2D1_POINT_2F* points, UINT count) noexcept override {
    for (UINT i = 0; i < count; ++i) {
      current_ = map(points[i]);
      curve(1, {current_});
    }
  }

  void STDMETHODCALLTYPE AddBeziers(const D2D1_BEZIER_SEGMENT* segments,
                                    UINT count) noexcept override {
    for (UINT i = 0; i < count; ++i) {
      const auto& s = segments[i];
      const point a = map(s.point1), b = map(s.point2), c = map(s.point3);
      if (cubic_) curve(3, {a, b, c});
      else quadratic(current_, a, b, c, 0);
      current_ = c;
    }
  }

  void STDMETHODCALLTYPE EndFigure(D2D1_FIGURE_END end) noexcept override {
    if (failed_) return;
    if (end == D2D1_FIGURE_END_CLOSED &&
        (current_.x != start_.x || current_.y != start_.y))
      curve(1, {start_});
    if (failed_) return;
    const u32 bytes = static_cast<u32>(data_.size() - polygon_);
    memcpy(data_.data() + polygon_, &bytes, sizeof(bytes));
  }

  HRESULT STDMETHODCALLTYPE Close() noexcept override { return failed_ ? E_OUTOFMEMORY : S_OK; }
  const std::vector<u8>& data() const { return data_; }

 private:
  point map(D2D1_POINT_2F p) const { return apply(matrix_, {p.x, p.y}); }

  template<class T>
  void append(const T& value) {
    constexpr size_t limit = 1024 * 1024;
    if (failed_ || data_.size() > limit - sizeof(T)) {
      failed_ = true;
      return;
    }
    try {
      const auto* bytes = reinterpret_cast<const u8*>(&value);
      data_.insert(data_.end(), bytes, bytes + sizeof(T));
    } catch (...) {
      failed_ = true;
    }
  }

  void curve(u16 type, std::initializer_list<point> points) {
    append(curve_header{type, static_cast<u16>(points.size())});
    for (point p : points) append(to_fixed(p));
  }

  void quadratic(point p0, point p1, point p2, point p3, unsigned depth) {
    if (failed_) return;
    const point a{p0.x + 1.5f * (p1.x - p0.x), p0.y + 1.5f * (p1.y - p0.y)};
    const point b{p3.x + 1.5f * (p2.x - p3.x), p3.y + 1.5f * (p2.y - p3.y)};
    if (depth == 10 || (std::abs(a.x - b.x) <= .125f && std::abs(a.y - b.y) <= .125f)) {
      curve(2, {midpoint(a, b), p3});
      return;
    }
    const point ab = midpoint(p0, p1), bc = midpoint(p1, p2), cd = midpoint(p2, p3);
    const point left = midpoint(ab, bc), right = midpoint(bc, cd);
    const point middle = midpoint(left, right);
    quadratic(p0, ab, left, middle, depth + 1);
    quadratic(middle, right, cd, p3, depth + 1);
  }

  DWRITE_MATRIX matrix_{};
  bool cubic_;
  bool failed_ = false;
  ULONG references_ = 1;
  size_t polygon_ = 0;
  point current_{}, start_{};
  std::vector<u8> data_;
};

IDWriteFactory* factory() {
  static const auto instance = [] {
    ComPtr<IDWriteFactory> result;
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(result.GetAddressOf()));
    return result;
  }();
  return instance.Get();
}

ComPtr<IDWriteFontFace> select_face(const LOGFONTW& spec) {
  ComPtr<IDWriteFontFace> face;
  IDWriteFactory* factory_ptr = factory();
  if (!factory_ptr) return face;
  ComPtr<IDWriteGdiInterop> interop;
  ComPtr<IDWriteFont> font;
  if (spec.lfFaceName[0] && SUCCEEDED(factory_ptr->GetGdiInterop(&interop)) &&
      SUCCEEDED(interop->CreateFontFromLOGFONT(&spec, &font)) &&
      SUCCEEDED(font->CreateFontFace(&face))) return face;

  ComPtr<IDWriteFontCollection> fonts;
  if (FAILED(factory_ptr->GetSystemFontCollection(&fonts)) || !fonts->GetFontFamilyCount())
    return {};
  UINT32 index = 0;
  BOOL found = FALSE;
  for (const wchar_t* name : {spec.lfFaceName, L"Segoe UI", L"Arial"}) {
    if (name[0] && SUCCEEDED(fonts->FindFamilyName(name, &index, &found)) && found)
      break;
  }
  if (!found) index = 0;
  ComPtr<IDWriteFontFamily> family;
  const auto weight = static_cast<DWRITE_FONT_WEIGHT>(
      spec.lfWeight ? std::clamp<LONG>(spec.lfWeight, 1, 999) : 400);
  const auto style = spec.lfItalic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL;
  if (FAILED(fonts->GetFontFamily(index, &family)) ||
      FAILED(family->GetFirstMatchingFont(weight, DWRITE_FONT_STRETCH_NORMAL, style, &font)) ||
      FAILED(font->CreateFontFace(&face))) return {};
  return face;
}

LOGFONTW read_font(const void* spec) {
  static_assert(sizeof(LOGFONTW) == logfontw_bytes);
  LOGFONTW font{};
  if (spec) memcpy(&font, spec, sizeof(font));
  font.lfFaceName[LF_FACESIZE - 1] = 0;
  return font;
}

struct font_cache {
  LOGFONTW font{};
  ComPtr<IDWriteFontFace> face;
};

ComPtr<IDWriteFontFace> cached_face(const LOGFONTW& spec) {
  thread_local font_cache cache;
  if (!cache.face || memcmp(&spec, &cache.font, sizeof(spec))) {
    cache.face = select_face(spec);
    cache.font = spec;
  }
  return cache.face;
}

bool write_metrics(const DWRITE_GLYPH_METRICS& glyph, float scale,
                   const DWRITE_MATRIX& m, glyph_metrics& result) {
  const float top = float(glyph.topSideBearing) - glyph.verticalOriginY;
  const float bottom = float(glyph.advanceHeight) + top - glyph.topSideBearing -
                       glyph.bottomSideBearing;
  const float left = float(glyph.leftSideBearing);
  const float right = float(glyph.advanceWidth) - glyph.rightSideBearing;
  const point points[] = {
      apply(m,{left*scale, top*scale}), apply(m,{right*scale, top*scale}),
      apply(m,{left*scale, bottom*scale}), apply(m,{right*scale, bottom*scale}),
  };
  float x_min = points[0].x, y_min = points[0].y;
  float x_max = x_min, y_max = y_min;
  for (point p : points) {
    x_min = std::min(x_min, p.x); x_max = std::max(x_max, p.x);
    y_min = std::min(y_min, p.y); y_max = std::max(y_max, p.y);
  }
  if (std::max({std::abs(x_min),std::abs(y_min),std::abs(x_max),std::abs(y_max)}) > 32766)
    return false;
  const auto advance = apply(m, {glyph.advanceWidth * scale, 0});
  if (std::abs(advance.x) > 32766 || std::abs(advance.y) > 32766) return false;
  result.advance_x = static_cast<i16>(std::lround(advance.x));
  result.advance_y = static_cast<i16>(std::lround(-advance.y));
  if (left >= right || top >= bottom) return true;
  result.origin_x = static_cast<i32>(std::floor(x_min));
  result.origin_y = -static_cast<i32>(std::floor(y_min));
  result.width = static_cast<u32>(std::ceil(x_max) - result.origin_x);
  result.height = static_cast<u32>(std::ceil(y_max) + result.origin_y);
  return true;
}

u32 render_bitmap(IDWriteFontFace* face, UINT16 glyph, float em,
                  const DWRITE_MATRIX& m, u32 kind, glyph_metrics& result,
                  u32 capacity, void* buffer) {
  const DWRITE_GLYPH_RUN run{face, em, 1, &glyph, nullptr, nullptr, FALSE, 0};
  const bool mono = kind == bitmap;
  const auto mode = mono ? DWRITE_RENDERING_MODE_ALIASED : DWRITE_RENDERING_MODE_GDI_CLASSIC;
  const auto texture = mono ? DWRITE_TEXTURE_ALIASED_1x1 : DWRITE_TEXTURE_CLEARTYPE_3x1;
  ComPtr<IDWriteGlyphRunAnalysis> analysis;
  if (FAILED(factory()->CreateGlyphRunAnalysis(&run, 1.f, &m, mode,
        DWRITE_MEASURING_MODE_GDI_CLASSIC, 0.f, 0.f, &analysis))) return error;
  RECT box{};
  if (FAILED(analysis->GetAlphaTextureBounds(texture, &box))) return error;
  const i64 width = i64(box.right) - box.left, height = i64(box.bottom) - box.top;
  if (width < 0 || height < 0 || width > 8192 || height > 8192 ||
      width * height > max_bitmap_pixels) return error;
  result.width = static_cast<u32>(width);
  result.height = static_cast<u32>(height);
  result.origin_x = box.left;
  result.origin_y = -box.top;
  const u32 bytes = pack_bitmap(kind, result.width, result.height, nullptr, 0, 0, nullptr);
  if (!buffer || !capacity || !bytes || bytes == error) return bytes;
  if (capacity < bytes) return error;
  const size_t pixels = size_t(width) * size_t(height);
  std::vector<u8> coverage(pixels * (mono ? 1 : 3));
  if (FAILED(analysis->CreateAlphaTexture(texture, &box, coverage.data(),
                                          static_cast<UINT32>(coverage.size())))) return error;
  if (!mono) {
    for (size_t i = 0; i < pixels; ++i)
      coverage[i] = static_cast<u8>((coverage[3*i] + coverage[3*i+1] + coverage[3*i+2] + 1) / 3);
  }
  return pack_bitmap(kind, result.width, result.height, coverage.data(), result.width,
                     capacity, static_cast<u8*>(buffer));
}

}  // namespace

u32 outline(const void* requested_font, u32 character, u32 flags,
            glyph_metrics* result, u32 capacity, void* buffer, const matrix* mat) {
  if (!result || !mat) return error;
  const u32 kind = flags & ~(glyph_index | unhinted);
  if (kind > gray8) return error;
  try {
    const auto spec = read_font(requested_font);
    auto face = cached_face(spec);
    if (!face) return error;
    UINT16 glyph = static_cast<UINT16>(character);
    if (flags & glyph_index) {
      if (character >= face->GetGlyphCount()) return error;
    } else if (character > 0x10ffff ||
               FAILED(face->GetGlyphIndices(&character, 1, &glyph))) {
      return error;
    }

    DWRITE_FONT_METRICS font{};
    face->GetMetrics(&font);
    if (!font.designUnitsPerEm) return error;
    const float height = std::abs(float(spec.lfHeight));
    float em = height ? std::min(height, 4096.f) : 16.f;
    const u32 cell = u32(font.ascent) + font.descent;
    if (spec.lfHeight > 0 && cell) em *= float(font.designUnitsPerEm) / float(cell);

    DWRITE_GLYPH_METRICS g{};
    const HRESULT metrics_hr = (flags & unhinted)
        ? face->GetDesignGlyphMetrics(&glyph, 1, &g)
        : face->GetGdiCompatibleGlyphMetrics(em, 1.f, nullptr, FALSE, &glyph, 1, &g);
    if (FAILED(metrics_hr)) return error;
    const auto m = transform(*mat);
    glyph_metrics value{};
    if (!write_metrics(g, em / font.designUnitsPerEm, m, value)) return error;

    u32 size = 0;
    if (kind == native || kind == bezier) {
      outline_sink sink(m, kind == bezier);
      if (FAILED(face->GetGlyphRunOutline(em, &glyph, nullptr, nullptr, 1,
                                         FALSE, FALSE, &sink)) || FAILED(sink.Close()))
        return error;
      const auto& data = sink.data();
      size = static_cast<u32>(data.size());
      if (buffer && capacity && size) {
        if (capacity < size) return error;
        memcpy(buffer, data.data(), size);
      }
    } else if (kind != metrics) {
      size = render_bitmap(face.Get(), glyph, em, m, kind, value, capacity, buffer);
      if (size == error) return error;
    }
    *result = value;
    return size;
  } catch (...) {
    return error;
  }
}

u32 indices(const void* requested_font, const u16* text, u32 count,
            u16* result, bool mark_missing) {
  if (!count) return 0;
  try {
    auto face = cached_face(read_font(requested_font));
    if (!face) return error;
    constexpr u32 batch_size = 128;
    for (u32 offset = 0; offset < count;) {
      const u32 batch = std::min(batch_size, count - offset);
      UINT32 codepoints[batch_size];
      for (u32 i = 0; i < batch; ++i) codepoints[i] = text[offset + i];
      if (FAILED(face->GetGlyphIndices(codepoints, batch, result + offset))) return error;
      if (mark_missing) {
        for (u32 i = 0; i < batch; ++i)
          if (!result[offset + i]) result[offset + i] = 0xffff;
      }
      offset += batch;
    }
    return count;
  } catch (...) {
    return error;
  }
}

#else

u32 outline(const void*, u32, u32, glyph_metrics*, u32, void*, const matrix*) {
  return error;
}

u32 indices(const void*, const u16*, u32, u16*, bool) { return error; }

#endif

}  // namespace trt_glyph
