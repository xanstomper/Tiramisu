#include "../src/trt/trt.h"
#include <cstdio>
#include <cstring>
int main() {
  char out[512]; int fail = 0;
  trt_vfs_mount("z:", "/games/foo");
  trt_vfs_mount("c:", "/games/foo/overlay");
  auto check=[&](const char* path, const char* want) {
    int rc=trt_vfs_host_to_guest(path,out,sizeof(out));
    if ((want && (rc || strcmp(out,want))) || (!want && rc==0)) {
      printf("FAIL path=%s rc=%d got=%s\n",path,rc,rc ? "<none>" : out); ++fail;
    }
  };
  check("/games/foo/Cuphead_Data/Mono/EmbedRuntime/mono.dll", "Z:\\Cuphead_Data\\Mono\\EmbedRuntime\\mono.dll");
  check("/games/foo/overlay/sub/a.dll", "C:\\sub\\a.dll");
  check("/games/foo2/a.dll", nullptr);
  check("/games/foo", "Z:\\");
  if (!trt_vfs_host_to_guest("/games/foo/sub/a.dll",out,4)) { puts("FAIL buffer overflow"); ++fail; }
  printf("module path mount checks: %s\n",fail ? "FAILED" : "PASS");
  return fail ? 1 : 0;
}
