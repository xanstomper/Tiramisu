// tiramisu/src/host/webxfer_resumable.h
// Resumable striped LAN upload metadata persistence, deterministic staging
// fingerprints, range coverage checks, and temporary staging artifact filters.
#ifndef TIRAMISU_WEBXFER_RESUMABLE_H
#define TIRAMISU_WEBXFER_RESUMABLE_H

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace longan {
namespace webxfer_detail {

inline std::string resume_fingerprint_hex(const std::string& scope,
                                          const std::string& key,
                                          uint64_t total_size) {
  uint64_t h = 14695981039346656037ull;
  auto mix = [&](const void* p, size_t n) {
    const auto* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) {
      h ^= static_cast<uint64_t>(b[i]);
      h *= 1099511628211ull;
    }
  };
  mix(scope.data(), scope.size());
  const uint8_t sep = 0x1Fu;
  mix(&sep, 1);
  mix(key.data(), key.size());
  mix(&sep, 1);
  mix(&total_size, sizeof(total_size));
  char out[17];
  snprintf(out, sizeof(out), "%016llx", static_cast<unsigned long long>(h));
  return std::string(out);
}

inline bool init_resume_meta(const std::string& meta_path, uint64_t total_size) {
  FILE* f = fopen(meta_path.c_str(), "wb");
  if (!f) return false;
  fprintf(f, "v1 %llu\n", static_cast<unsigned long long>(total_size));
  const bool ok = (fflush(f) == 0);
  fclose(f);
  return ok;
}

inline bool append_resume_range(const std::string& meta_path, uint64_t offset,
                                uint64_t len) {
  if (meta_path.empty() || len == 0) return false;
  FILE* f = fopen(meta_path.c_str(), "ab");
  if (!f) return false;
  fprintf(f, "%llu %llu\n", static_cast<unsigned long long>(offset),
          static_cast<unsigned long long>(len));
  const bool ok = (fflush(f) == 0);
  fclose(f);
  return ok;
}

inline bool load_resume_meta(const std::string& meta_path, uint64_t expected_total,
                             std::map<uint64_t, uint64_t>* out_ranges) {
  if (!out_ranges) return false;
  out_ranges->clear();
  FILE* f = fopen(meta_path.c_str(), "rb");
  if (!f) return false;
  char line[128];
  if (!fgets(line, sizeof(line), f)) {
    fclose(f);
    return false;
  }
  unsigned long long hdr_total = 0;
  if (sscanf(line, "v1 %llu", &hdr_total) != 1 ||
      static_cast<uint64_t>(hdr_total) != expected_total) {
    fclose(f);
    return false;
  }
  while (fgets(line, sizeof(line), f)) {
    unsigned long long off = 0, len = 0;
    if (sscanf(line, "%llu %llu", &off, &len) == 2 && len > 0 &&
        off <= expected_total && len <= expected_total - off) {
      auto& prev = (*out_ranges)[static_cast<uint64_t>(off)];
      if (static_cast<uint64_t>(len) > prev) prev = static_cast<uint64_t>(len);
    }
  }
  fclose(f);
  return true;
}

inline bool chunk_ranges_cover_total(const std::map<uint64_t, uint64_t>& ranges,
                                     uint64_t total) {
  if (total == 0) return true;
  uint64_t covered = 0;
  for (const auto& kv : ranges) {
    if (kv.first > covered) return false;
    const uint64_t end = kv.first + kv.second;
    if (end > covered) covered = end;
    if (covered >= total) return true;
  }
  return covered >= total;
}

inline uint64_t merged_range_bytes(const std::map<uint64_t, uint64_t>& ranges,
                                   std::vector<std::pair<uint64_t, uint64_t>>* out_merged = nullptr) {
  if (out_merged) out_merged->clear();
  uint64_t total = 0;
  bool have = false;
  uint64_t cur_s = 0, cur_e = 0;
  for (const auto& kv : ranges) {
    if (kv.second == 0) continue;
    const uint64_t s = kv.first;
    const uint64_t e = kv.first + kv.second;
    if (!have) {
      cur_s = s;
      cur_e = e;
      have = true;
    } else if (s <= cur_e) {
      if (e > cur_e) cur_e = e;
    } else {
      total += (cur_e - cur_s);
      if (out_merged) out_merged->emplace_back(cur_s, cur_e - cur_s);
      cur_s = s;
      cur_e = e;
    }
  }
  if (have) {
    total += (cur_e - cur_s);
    if (out_merged) out_merged->emplace_back(cur_s, cur_e - cur_s);
  }
  return total;
}

inline std::string format_completed_ranges_json(
    const std::map<uint64_t, uint64_t>& ranges) {
  std::vector<std::pair<uint64_t, uint64_t>> merged;
  merged_range_bytes(ranges, &merged);
  std::string out = "[";
  char buf[64];
  for (size_t i = 0; i < merged.size(); ++i) {
    if (i) out += ",";
    snprintf(buf, sizeof(buf), "[%llu,%llu]",
             static_cast<unsigned long long>(merged[i].first),
             static_cast<unsigned long long>(merged[i].second));
    out += buf;
  }
  out += "]";
  return out;
}

inline bool is_temp_upload_artifact(const std::string& filename) {
  return filename.rfind(".tiramisu-resume-", 0) == 0 ||
         filename.rfind(".tiramisu-chunk-", 0) == 0 ||
         filename.rfind(".tiramisu-upload-", 0) == 0 ||
         filename.rfind(".chunk-", 0) == 0;
}

}  // namespace webxfer_detail
}  // namespace longan

#endif
