#!/usr/bin/env bash
# tiramisu/tools/edge-spoof.sh — build the experimental Edge-identity package.
#
# This is the one-command entry point for the Xbox hardware-mouse experiment in
# XBOX-MOUSE.md. It builds the host into a SEPARATE output dir (default
# out-edgespoof) and packs it with the Microsoft Edge identity + a matching
# self-signed cert. The normal `out/` build and its dev.cer are never touched.
#
#   tools/edge-spoof.sh [out-dir]           # build host + spoofed package
#   SKIP_BUILD=1 tools/edge-spoof.sh out-mousefix   # reuse an existing host build
#
# Deploy to your own Dev Mode console with the printed command, then watch the
# host log for `input: physical mouse button received`. If you see
# `input: GameInput mouse callback failed hr=0x80004001` the allowlist did not
# bite. See XBOX-MOUSE.md for the full premise and limits.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"; cd "$here"
out="${1:-out-edgespoof}"

if [[ "${SKIP_BUILD:-0}" == "1" ]]; then
  [[ -x "$out/layout/tiramisu.exe" ]] \
    || { echo "ERROR: no built host in $out/layout — run without SKIP_BUILD=1 first" >&2; exit 2; }
  echo "SKIP_BUILD=1: reusing $out/layout/tiramisu.exe"
else
  "$here/tools/build-host.sh" "$out"
fi

EDGE_SPOOF=1 "$here/tools/pack.sh" "$out"
