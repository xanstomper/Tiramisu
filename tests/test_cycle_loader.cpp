// Verify that the loader binds and seals the entire graph before initializing DLLs.
#define main libload_existing_test_main
#include "test_libload.cpp"
#undef main
#include <string>

static int protections_until_failure = -1;

static int cycle_test_protect(void* base, size_t size, unsigned protection) {
  if (protections_until_failure == 0) return -1;
  if (protections_until_failure > 0) --protections_until_failure;
  return linux_protect(base, size, protection);
}

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  const std::string root = argv[1];
  trt_backend be{&linux_alloc, &cycle_test_protect, &linux_release, &linux_log, nullptr};
  trt_runtime_init(&be, argv[1]);
  trt_bridge_boot(&be);

  const auto before = trt_mem_budget_committed();
  const std::string missing = root + "/missing/cycle_a.dll";
  trt_image* failed = trt_load_pe(missing.c_str(), "cycle_a.dll", 1);
  CHECK(failed == nullptr && log_has("unresolved cyclic DLL import: cycle_a.dll!cycle_a"),
        "missing cyclic export produces a named load failure");
  CHECK(trt_images_count() == 0 && trt_mem_budget_committed() == before,
        "failed cyclic import releases both staged images");

  const std::string valid = root + "/cycle_a.dll";
  protections_until_failure = 3;
  failed = trt_load_pe(valid.c_str(), "cycle_a.dll", 1);
  CHECK(failed == nullptr && trt_images_count() == 0 &&
        trt_mem_budget_committed() == before,
        "a protection failure rolls back the entire graph");
  CHECK(!log_has("entry returned"), "failed graphs do not run DLL initializers");

  protections_until_failure = -1;
  trt_image* image = trt_load_pe(valid.c_str(), "cycle_a.dll", 1);
  CHECK(image != nullptr && trt_images_find("cycle_a") == image &&
        trt_images_find("cycle_b.dll") != nullptr,
        "both cyclic DLLs are registered after a successful retry");
  CHECK(log_has("cycle_b.dll entry returned 0x1") &&
        log_has("cycle_a.dll entry returned 0x1"),
        "both initializers can call the other's sealed exports");

  printf("RESULT: %s\n", failures ? "FAILURES" : "ALL PASS");
  return failures ? 1 : 0;
}
