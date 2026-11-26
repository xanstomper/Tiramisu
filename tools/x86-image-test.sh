#!/usr/bin/env bash
# tiramisu/tools/x86-image-test.sh — PE32 image execution gate.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
cd "$here"
mkdir -p /tmp/opencode/x86-image-test
clang++ -std=c++17 -O2 -Wall -no-pie -I src/trt \
  tests/test_x86_image.cpp src/trt/x86_32.cpp src/trt/x86_image.cpp \
  src/trt/images.cpp src/trt/pe_loader.cpp src/trt/module_table.cpp \
  src/trt/apiset.cpp src/trt/pe_exports.cpp src/trt/thunk.cpp \
  src/trt/vfs.cpp src/trt/iso.cpp \
  /tmp/opencode/x86-image-test/miniz_tinfl.o \
  -o /tmp/opencode/x86-image-test/test_x86_image 2>/dev/null || {
  clang -O2 -Wall -fno-pie -c src/host/vendor/miniz_tinfl.c \
    -o /tmp/opencode/x86-image-test/miniz_tinfl.o
  clang++ -std=c++17 -O2 -Wall -no-pie -I src/trt \
    tests/test_x86_image.cpp src/trt/x86_32.cpp src/trt/x86_image.cpp \
    src/trt/images.cpp src/trt/pe_loader.cpp src/trt/module_table.cpp \
    src/trt/apiset.cpp src/trt/pe_exports.cpp src/trt/thunk.cpp \
    src/trt/vfs.cpp src/trt/iso.cpp \
    /tmp/opencode/x86-image-test/miniz_tinfl.o \
    -o /tmp/opencode/x86-image-test/test_x86_image
}
/tmp/opencode/x86-image-test/test_x86_image
