// Regression: cyclic sibling imports must fail without recursive remapping.
#define main libload_existing_test_main
#include "test_libload.cpp"
#undef main

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  trt_backend be{&linux_alloc, &linux_protect, &linux_release, &linux_log, nullptr};
  trt_runtime_init(&be, argv[2]);
  trt_bridge_boot(&be);
  trt_image* image = trt_load_pe(argv[1], "cycle_a.dll", 0);
  CHECK(image == nullptr, "cycle is rejected until two-phase binding is supported");
  CHECK(trt_images_find("cycle_a.dll") == nullptr &&
        trt_images_find("cycle_b.dll") == nullptr,
        "neither image is registered after a rejected cycle");
  CHECK(log_has("cyclic guest DLL import: cycle_a.dll"),
        "cycle is identified by the shared load guard");
  printf("RESULT: %s\n", failures ? "FAILURES" : "ALL PASS");
  return failures ? 1 : 0;
}
