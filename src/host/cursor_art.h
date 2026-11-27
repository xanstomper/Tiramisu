#pragma once

namespace longan {

// Scale with the presentation surface. A fixed minimum on small guest
// surfaces becomes a large arrow when the console expands them to the TV.
constexpr int cursor_height(int frame_height) {
  if (frame_height <= 0) return 0;
  if (frame_height >= 2160) return 32;
  const int height = (frame_height * 16 + 540) / 1080;
  return height > 0 ? height : 1;
}

}  // namespace longan
