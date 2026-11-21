// Regression: cyclic sibling imports must fail without recursive remapping.
#define main libload_existing_test_main
#include "test_libload.cpp"
#undef main
#include <string>

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  trt_backend be{&linux_alloc, &linux_protect, &linux_release, &linux_log, nullptr};
  trt_runtime_init(&be, argv[2]);
  trt_bridge_boot(&be);
  trt_image* image = trt_load_pe(argv[1], "cycle_a.dll", 1);
  CHECK(image != nullptr, "cycle_a maps with its cyclic dependency");
  CHECK(trt_images_find("cycle_b.dll") != nullptr, "cycle_b is registered");
  CHECK(log_has("cycle_b.dll entry returned 0x1") &&
        log_has("cycle_a.dll entry returned 0x1"),
        "both cyclic DLL initializers call each other's sealed exports");
  trt_images_reset();
  std::string bad_path = std::string(argv[2]) + "/cycle_bad_a.dll";
  trt_image* failed = trt_load_pe(bad_path.c_str(), "cycle_bad_a.dll", 0);
  CHECK(failed == nullptr, "missing dependency rejects graph");
  CHECK(trt_images_find("cycle_bad_b.dll") == nullptr,
        "failed graph removes dependent DLL that references failed image");
  printf("RESULT: %s\n", failures ? "FAILURES" : "ALL PASS");
  return failures ? 1 : 0;
}
