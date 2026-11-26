#!/usr/bin/env bash
# tiramisu/tools/x86-test.sh — standalone gate for the clean-room x86-32 core.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
cd "$here"
mkdir -p /tmp/opencode/x86-test
clang++ -std=c++17 -O2 -Wall -no-pie -I src/trt \
  tests/test_x86_32.cpp src/trt/x86_32.cpp \
  -o /tmp/opencode/x86-test/test_x86_32
/tmp/opencode/x86-test/test_x86_32
