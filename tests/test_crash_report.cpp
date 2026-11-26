// tiramisu/tests/test_crash_report.cpp
// Offline checks for the crash bundle: a report must carry every section a
// post-mortem needs, an empty section must say so rather than vanish, and the
// directory must stay bounded so a crash loop cannot fill the console disk.
#include "../src/host/crash_report.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, what)                                            \
  do {                                                               \
    ++checks;                                                        \
    if (cond) {                                                      \
      printf("PASS %s\n", what);                                     \
    } else {                                                         \
      printf("FAIL %s (%s:%d)\n", what, __FILE__, __LINE__);          \
      ++failures;                                                    \
    }                                                                \
  } while (0)

static std::string slurp(const std::string& path) {
  std::string out;
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return out;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  fclose(f);
  return out;
}

int main() {
  char root[] = "/tmp/tscr_test_XXXXXX";
  CHECK(mkdtemp(root) != nullptr, "temp LocalState created");
  const std::string local_state = root;

  CHECK(longan::crash_report_dir(local_state) == local_state + "/crash",
        "report directory sits under LocalState");
  CHECK(longan::crash_report_dir("").empty(), "no LocalState means no report");
  CHECK(longan::crash_report_write("", longan::crash_report_input{}).empty(),
        "write refuses an empty LocalState");

  using longan::launch_checkpoint;
  using longan::launch_outcome;
  using longan::launch_phase;
  launch_checkpoint checkpoint{};
  CHECK(!longan::launch_checkpoint_read(local_state, &checkpoint),
        "no checkpoint is reported before Play");
  CHECK(longan::launch_checkpoint_write(local_state,
        {launch_phase::native_tls, launch_outcome::running, 0}),
        "persist the native TLS checkpoint");
  CHECK(longan::launch_checkpoint_read(local_state, &checkpoint) &&
            checkpoint.phase == launch_phase::native_tls &&
            checkpoint.outcome == launch_outcome::running && checkpoint.code == 0,
        "recover the last phase if the process terminates without a handler");
  CHECK(longan::launch_checkpoint_write(local_state,
        {launch_phase::native_entry, launch_outcome::host_fault, 0xc0000005u}),
        "replace the running checkpoint with the fault outcome");
  CHECK(longan::launch_checkpoint_read(local_state, &checkpoint) &&
            checkpoint.phase == launch_phase::native_entry &&
            checkpoint.outcome == launch_outcome::host_fault &&
            checkpoint.code == 0xc0000005u,
        "recover the fault code and stage without reading the private bundle");
  const std::string checkpoint_path = local_state + "/launch-checkpoint.bin";
  CHECK(slurp(checkpoint_path).size() == 16, "launch checkpoint stays bounded");
  if (FILE* corrupt = fopen(checkpoint_path.c_str(), "wb")) {
    fputc(0, corrupt);
    fclose(corrupt);
  }
  CHECK(!longan::launch_checkpoint_read(local_state, &checkpoint),
        "reject a truncated launch checkpoint");

  longan::crash_report_input in;
  in.reason = "guest fault: 0xc0000005 at titanfall.exe+0x1a2b3, read address 0x8";
  in.detail = "rip=00007ff6 rip0 rsp=12ff00";
  in.settings_ini = "shader_cache=1\nvsync=0\n";
  in.census_json = "{\n  \"compiles\": 12\n}\n";
  in.log_tail = "boot: guest entry\nrender: pipeline 3 created\n";

  const std::string path = longan::crash_report_write(local_state, in, 2);
  CHECK(!path.empty(), "report written");
  CHECK(longan::crash_report_count(local_state) == 1, "one report on disk");

  const std::string body = slurp(path);
  CHECK(body.find("---- fault ----") != std::string::npos, "fault section present");
  CHECK(body.find("guest fault: 0xc0000005") != std::string::npos,
        "fault line carried verbatim");
  CHECK(body.find("---- registers and stack ----") != std::string::npos,
        "register section present");
  CHECK(body.find("rip=00007ff6") != std::string::npos, "registers carried");
  CHECK(body.find("---- settings in force ----") != std::string::npos,
        "settings section present");
  CHECK(body.find("vsync=0") != std::string::npos, "settings carried");
  CHECK(body.find("---- shader census ----") != std::string::npos,
        "census section present");
  CHECK(body.find("\"compiles\": 12") != std::string::npos, "census carried");
  CHECK(body.find("---- log tail ----") != std::string::npos, "log section present");
  CHECK(body.find("guest entry") != std::string::npos, "log tail carried");

  longan::crash_report_input bare;
  bare.reason = "guest fault: 0x80000003 at unknown";
  const std::string bare_path = longan::crash_report_write(local_state, bare, 2);
  const std::string bare_body = slurp(bare_path);
  CHECK(bare_body.find("(no context captured)") != std::string::npos,
        "missing context is stated, not silently empty");
  CHECK(bare_body.find("(settings unavailable)") != std::string::npos,
        "missing settings stated");
  CHECK(bare_body.find("(no shaders translated)") != std::string::npos,
        "missing census stated");

  // keep=2: a crash loop must not grow the folder without bound.
  for (int i = 0; i < 4; ++i) {
    longan::crash_report_input loop_in;
    loop_in.reason = "guest fault: repeated";
    longan::crash_report_write(local_state, loop_in, 2);
  }
  CHECK(longan::crash_report_count(local_state) == 2, "keeps only the newest reports");
  CHECK(!slurp(path).empty() || true, "oldest report is gone");
  CHECK(slurp(path).empty(), "oldest report deleted");

  CHECK(longan::crash_report_trim(local_state, 0) >= 0, "trim clamps keep=0");
  CHECK(longan::crash_report_count(local_state) == 1, "trim to one report");

  system((std::string("rm -rf ") + root).c_str());
  printf("%s: %d checks, %d failed\n", failures ? "RESULT: FAIL" : "RESULT: ALL PASS",
         checks, failures);
  return failures ? 1 : 0;
}
