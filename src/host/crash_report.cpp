// tiramisu/src/host/crash_report.cpp
// One-file crash bundles (see crash_report.h).
#include "crash_report.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace longan {
namespace {

#if defined(_WIN32)
constexpr const char* k_sep = "\\";
#else
constexpr const char* k_sep = "/";
#endif

bool make_dirs(const std::string& path) {
  if (path.empty()) return false;
  std::string sofar;
  for (size_t i = 0; i <= path.size(); ++i) {
    const bool end = (i == path.size());
    if (!end && path[i] != '/' && path[i] != '\\') continue;
    sofar = path.substr(0, i);
    if (sofar.empty()) continue;
#if defined(_WIN32)
    if (_mkdir(sofar.c_str()) != 0 && errno != EEXIST) return false;
#else
    if (mkdir(sofar.c_str(), 0755) != 0 && errno != EEXIST) return false;
#endif
  }
  return true;
}

// Report file names are report-<n>.txt with n starting at 1; -1 for anything
// else so unrelated files in the folder are left alone.
int report_index(const std::string& name) {
  if (name.size() < 12) return -1;
  if (name.compare(0, 7, "report-") != 0) return -1;
  if (name.compare(name.size() - 4, 4, ".txt") != 0) return -1;
  int n = 0;
  for (size_t i = 7; i + 4 < name.size(); ++i) {
    if (name[i] < '0' || name[i] > '9') return -1;
    n = n * 10 + (name[i] - '0');
    if (n > 100000) return -1;
  }
  return n;
}

std::vector<int> report_indices(const std::string& dir) {
  std::vector<int> out;
#if defined(_WIN32)
  const std::string pattern = dir + k_sep + "report-*.txt";
  struct _finddata_t fd;
  intptr_t h = _findfirst(pattern.c_str(), &fd);
  if (h == -1) return out;
  do {
    const int n = report_index(fd.name);
    if (n > 0) out.push_back(n);
  } while (_findnext(h, &fd) == 0);
  _findclose(h);
#else
  DIR* d = opendir(dir.c_str());
  if (!d) return out;
  while (struct dirent* e = readdir(d)) {
    const int n = report_index(e->d_name);
    if (n > 0) out.push_back(n);
  }
  closedir(d);
#endif
  std::sort(out.begin(), out.end());
  return out;
}

std::string report_path(const std::string& local_state, int index) {
  char name[32];
  snprintf(name, sizeof(name), "report-%d.txt", index);
  return crash_report_dir(local_state) + k_sep + name;
}

// Sections are appended in a fixed order so two reports from different builds
// can be diffed; empty sections say so instead of vanishing.
void section(std::string* out, const char* title, const std::string& body,
             const char* empty) {
  *out += "---- ";
  *out += title;
  *out += " ----\n";
  if (body.empty()) {
    *out += empty;
    *out += "\n";
  } else {
    *out += body;
    if (body[body.size() - 1] != '\n') *out += '\n';
  }
}

}  // namespace

std::string crash_report_dir(const std::string& local_state) {
  if (local_state.empty()) return std::string();
  return local_state + k_sep + "crash";
}

std::string crash_report_write(const std::string& local_state,
                               const crash_report_input& in, int keep) {
  const std::string dir = crash_report_dir(local_state);
  if (dir.empty()) return std::string();
  if (!make_dirs(dir)) return std::string();

  const std::vector<int> existing = report_indices(dir);
  const int index = existing.empty() ? 1 : existing.back() + 1;
  const std::string path = report_path(local_state, index);

  std::string text = "tiramisu crash report\n";
  section(&text, "fault", in.reason, "(no reason recorded)");
  section(&text, "registers and stack", in.detail, "(no context captured)");
  section(&text, "settings in force", in.settings_ini, "(settings unavailable)");
  section(&text, "shader census", in.census_json, "(no shaders translated)");
  section(&text, "log tail", in.log_tail, "(log unavailable)");

  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return std::string();
  const size_t wrote = fwrite(text.data(), 1, text.size(), f);
  const bool closed = fclose(f) == 0;
  if (!closed || wrote != text.size()) {
    remove(path.c_str());
    return std::string();
  }
  crash_report_trim(local_state, keep);
  return path;
}

int crash_report_count(const std::string& local_state) {
  const std::string dir = crash_report_dir(local_state);
  if (dir.empty()) return 0;
  return (int)report_indices(dir).size();
}

int crash_report_trim(const std::string& local_state, int keep) {
  const std::string dir = crash_report_dir(local_state);
  if (dir.empty()) return 0;
  if (keep < 1) keep = 1;
  const std::vector<int> all = report_indices(dir);
  if ((int)all.size() <= keep) return 0;
  int removed = 0;
  const size_t drop = all.size() - (size_t)keep;
  for (size_t i = 0; i < drop; ++i) {
    const std::string p = report_path(local_state, all[i]);
    if (remove(p.c_str()) == 0) ++removed;
  }
  return removed;
}

static bool write_checkpoint(const std::string& local_state, const char* filename,
                             const launch_checkpoint& report,
                             std::atomic_flag& writer) noexcept {
  // A fatal error may interrupt a normal checkpoint write. Keep the last
  // complete checkpoint readable and avoid blocking the exception handler.
  if (local_state.empty() || writer.test_and_set()) return false;
  struct release_writer {
    std::atomic_flag& value;
    ~release_writer() { value.clear(); }
  } release{writer};
  try {
    const std::string path = local_state + k_sep + filename;
    const std::string temporary = path + ".tmp";
    FILE* file = fopen(temporary.c_str(), "wb");
    if (!file) return false;
    const uint32_t fields[] = {0x31524354u, static_cast<uint32_t>(report.phase),
                              static_cast<uint32_t>(report.outcome), report.code};
    const bool written = fwrite(fields, sizeof(fields), 1, file) == 1;
    const bool closed = fclose(file) == 0;
    if (written && closed) {
#ifdef _WIN32
      if (MoveFileExA(temporary.c_str(), path.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
#else
      if (rename(temporary.c_str(), path.c_str()) == 0) return true;
#endif
    }
    remove(temporary.c_str());
  } catch (...) {}
  return false;
}

static bool read_checkpoint(const std::string& local_state, const char* filename,
                            launch_checkpoint* report) noexcept {
  if (local_state.empty() || !report) return false;
  try {
    const std::string path = local_state + k_sep + filename;
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) return false;
    uint32_t fields[4]{};
    const bool read = fread(fields, sizeof(fields), 1, file) == 1;
    const bool exact = fgetc(file) == EOF && !ferror(file);
    fclose(file);
    if (!read || !exact || fields[0] != 0x31524354u ||
        fields[1] > static_cast<uint32_t>(launch_phase::managed_runtime) ||
        fields[2] > static_cast<uint32_t>(launch_outcome::host_exit)) return false;
    *report = {static_cast<launch_phase>(fields[1]),
               static_cast<launch_outcome>(fields[2]), fields[3]};
    return true;
  } catch (...) {
    return false;
  }
}

bool launch_checkpoint_write(const std::string& local_state,
                             const launch_checkpoint& report) noexcept {
  static std::atomic_flag writer = ATOMIC_FLAG_INIT;
  return write_checkpoint(local_state, "launch-checkpoint.bin", report, writer);
}

bool launch_checkpoint_read(const std::string& local_state,
                            launch_checkpoint* report) noexcept {
  return read_checkpoint(local_state, "launch-checkpoint.bin", report);
}

bool launch_worker_fault_write(const std::string& local_state,
                               const launch_checkpoint& report) noexcept {
  static std::atomic_flag writer = ATOMIC_FLAG_INIT;
  return write_checkpoint(local_state, "launch-worker-fault.bin", report, writer);
}

bool launch_worker_fault_read(const std::string& local_state,
                              launch_checkpoint* report) noexcept {
  return read_checkpoint(local_state, "launch-worker-fault.bin", report);
}

}  // namespace longan
