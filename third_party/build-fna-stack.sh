#!/usr/bin/env bash
# third_party/build-fna-stack.sh — build the whole Path B managed stack:
# FNA-as-MS-names + forwarders, FNA3D, FAudio, and the REAL FMOD +
# Steamworks dllmap libraries (third_party/mono-bridges). The old
# spike-parity stubs (third_party/fna-stubs) were removed in Path B
# step 4 — the real bridges replaced them. Each sub-build is idempotent;
# pass --force to rebuild everything. Used by tools/build-test.sh
# section 45 (Mono/Celeste) and directly by developers.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
bash "$here/fna/build-fna.sh" "$@"
bash "$here/fna3d/build-fna3d.sh" "$@"
bash "$here/faudio/build-faudio.sh" "$@"
bash "$here/mono-bridges/build-real-bridges.sh" "$@"
echo "fna-stack: ALL BUILT"
