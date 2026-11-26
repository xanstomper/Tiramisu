#!/usr/bin/env bash
# tiramisu/tools/mscoree-test.sh — standalone gate for the real mscoree shim.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
cd "$here"
mkdir -p /tmp/opencode/mscoree-test
clang++ -std=c++17 -O2 -Wall -no-pie -I src/trt \
  tests/test_mscoree.cpp src/trt/module_table.cpp src/trt/apiset.cpp \
  -o /tmp/opencode/mscoree-test/test_mscoree
/tmp/opencode/mscoree-test/test_mscoree
