#!/usr/bin/env bash
# tiramisu/tools/host-autopilot-test.sh
# Standalone gate for the main.cpp-side autopilot state (header-only, no UWP
# headers needed), so it can run on the Linux build host without build-host.sh.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
cd "$here"
mkdir -p tests/out
clang++ -std=c++17 -O2 -Wall -no-pie tests/test_host_autopilot.cpp \
  -o tests/out/test_host_autopilot
tests/out/test_host_autopilot
