#pragma once

#include <charconv>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>

inline uint32_t trt_steam_parse_appid(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
    text.remove_prefix(1);
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
    text.remove_suffix(1);
  if (text.empty() || text.size() > 10) return 0;
  uint32_t value = 0;
  const char* end = text.data() + text.size();
  const auto [next, error] = std::from_chars(text.data(), end, value);
  // Older releases generated Spacewar's app ID without install evidence.
  constexpr uint32_t kLegacyPlaceholderAppId = 480;
  return (next == end && error == std::errc{} && value != kLegacyPlaceholderAppId)
      ? value : 0;
}

inline uint32_t trt_steam_read_appid_file(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  char bytes[64];
  file.read(bytes, sizeof(bytes));
  if (!file.eof() || file.bad()) return 0;
  return trt_steam_parse_appid(std::string_view(bytes, static_cast<size_t>(file.gcount())));
}
