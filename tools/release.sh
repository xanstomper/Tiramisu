#!/usr/bin/env bash
# tiramisu/tools/release.sh — build the host once and pack both release flavors.
#
#   tools/release.sh [out-dir]           # normal + KBM (Edge identity)
#   NO_SPOOF=1 tools/release.sh          # normal only
#   SPOOF_ONLY=1 tools/release.sh        # KBM only
#
# Output (default out/release):
#   Tiramisu.msix + dev.cer                     public Xan.Tiramisu identity
#   Tiramisu-EdgeSpoof.msix + edge-dev.cer      KBM / hardware-mouse experiment
#   SHA256SUMS
#
# The Edge-identity package is shipped in the same release on purpose: public
# builds cannot unlock Xbox hardware mouse passthrough, so KBM users need that
# artifact. It remains an unsupported, own-console Dev Mode experiment; see
# XBOX-MOUSE.md. Never remove it from a release without an equivalent KBM path.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"; cd "$here"
out="${1:-out/release}"

mkdir -p "$out"
"$here/tools/build-host.sh" "$out"

if [[ "${SPOOF_ONLY:-0}" != "1" ]]; then
  "$here/tools/pack.sh" "$out"
fi
if [[ "${NO_SPOOF:-0}" != "1" ]]; then
  EDGE_SPOOF=1 "$here/tools/pack.sh" "$out"
fi

# Keep the release directory self-describing.
{
  echo "# Tiramisu $out release artifacts"
  echo "# Generated $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "# Normal: Xan.Tiramisu / CN=TiramisuDev"
  echo "# KBM:    Microsoft.MicrosoftEdge experiment / CN=Microsoft Corporation"
} > "$out/RELEASE.txt"
( cd "$out" && sha256sum ./*.msix ./*.cer 2>/dev/null > SHA256SUMS || true )

echo
echo "---- release artifacts ----"
ls -l "$out"/*.msix "$out"/*.cer "$out"/SHA256SUMS 2>/dev/null || ls -l "$out"
echo
echo "KBM users install Tiramisu-EdgeSpoof.msix + edge-dev.cer."
echo "All other users install Tiramisu.msix + dev.cer."
