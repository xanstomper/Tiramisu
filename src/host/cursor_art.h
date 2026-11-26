#pragma once

namespace longan {

// Small arrow measured in output pixels. A 1080-line frame uses 18 pixels.
constexpr int cursor_height(int frame_height) {
  const int height = frame_height / 60;
  return height < 9 ? 9 : height > 32 ? 32 : height;
}

}  // namespace longan
