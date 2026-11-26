// tiramisu/src/host/crash_report.h
// One-file crash bundles for the console.
//
// On Xbox there is no debugger and no way to attach one, and the log tail is
// the only thing that survives a fatal fault. The settings toggle
// `crash_report` existed with nothing behind it; this is what it turns on: a
// single text file in LocalState\crash that a user can pull over Device Portal
// and that contains every fact a post-mortem needs (fault, registers, stack,
// settings in force, shader census, log tail) without asking them to
// reproduce anything.
#ifndef TIRAMISU_HOST_CRASH_REPORT_H
#define TIRAMISU_HOST_CRASH_REPORT_H

#include <cstdint>
#include <string>

namespace longan {

struct crash_report_input {
  std::string reason;       // one line: fault code, pc, guest or host
  std::string detail;       // registers / stack / caller list, may be empty
  std::string settings_ini; // settings in force at the fault
  std::string census_json;  // shader census (what the title was asking for)
  std::string log_tail;     // recent log lines, before the fault
};

// LocalState\crash directory the reports live in.
std::string crash_report_dir(const std::string& local_state);

// Write the next report into that directory and trim the directory to `keep`
// newest reports. Returns the path written, empty on failure (no throw: this
// runs inside a fault handler).
std::string crash_report_write(const std::string& local_state,
                               const crash_report_input& in, int keep = 8);

// Number of report-*.txt files present (offline tests use it).
int crash_report_count(const std::string& local_state);

// Delete all but the newest `keep` reports. Returns the number removed.
int crash_report_trim(const std::string& local_state, int keep);

// Bounded, numeric Play checkpoint. This is safe to expose independently
// of the private crash bundle and can survive a process exit.
enum class launch_phase : uint32_t { none, preparing, mounted, native_tls,
                                     native_entry, x86_execution, managed_runtime };
enum class launch_outcome : uint32_t { none, running, returned, guest_exit,
                                       guest_fault, host_fault, host_abort, host_exit };
struct launch_checkpoint {
  launch_phase phase = launch_phase::none;
  launch_outcome outcome = launch_outcome::none;
  uint32_t code = 0;
};
bool launch_checkpoint_write(const std::string& local_state,
                             const launch_checkpoint& report) noexcept;
bool launch_checkpoint_read(const std::string& local_state,
                            launch_checkpoint* report) noexcept;
bool launch_worker_fault_write(const std::string& local_state,
                               const launch_checkpoint& report) noexcept;
bool launch_worker_fault_read(const std::string& local_state,
                              launch_checkpoint* report) noexcept;

}  // namespace longan

#endif
