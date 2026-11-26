#!/bin/bash
# tiramisu/tools/pack.sh — package + sign + verify (proven recipe).
#
# Normal build:  manifest Identity = Xan.Tiramisu / CN=TiramisuDev
#                -> out/Tiramisu.msix + out/dev.cer
# KBM edition:   EDGE_SPOOF=1 (or --edge-spoof)
#                -> out/Tiramisu-EdgeSpoof.msix + out/edge-dev.cer
#                (experimental Microsoft Edge identity; see XBOX-MOUSE.md)
#
# Self-signed cert subject MUST equal the manifest Publisher. Verify =
# osslsigncode with trust anchor + makemsix round-trip unpack + identity grep.
# Any doubt fails the build here, never on the TV.
set -euo pipefail
export PATH="$HOME/.local/bin:$PATH"
here="$(cd "$(dirname "$0")/.." && pwd)"; cd "$here"

# ---- arguments --------------------------------------------------------------
# Accept the output dir in any position plus --edge-spoof / --no-spoof.
args=()
for a in "$@"; do
  case "$a" in
    --edge-spoof) EDGE_SPOOF=1 ;;
    --no-spoof)   EDGE_SPOOF=0 ;;
    *)            args+=("$a") ;;
  esac
done
out="${args[0]:-out}"
spoof="${EDGE_SPOOF:-0}"

if [[ "$spoof" == "1" || "$spoof" == "true" ]]; then
  spoof=1
  pkg_name="Tiramisu-EdgeSpoof"
  cert_prefix="$out/edge-dev"
  cert_subj="$(python3 tools/edge_spoof.py --cert-subj)"
  cert_label="Edge identity (KBM edition)"
else
  spoof=0
  pkg_name="Tiramisu"
  cert_prefix="$out/dev"
  cert_subj="/CN=TiramisuDev"
  cert_label="Xan.Tiramisu"
fi

[[ -x "$out/layout/tiramisu.exe" ]] || { echo "ERROR: run build-host.sh first" >&2; exit 2; }

# ---- stage package root: manifest + exe + guest + assets ---------------------
if [[ "$spoof" == "1" ]]; then
  python3 tools/edge_spoof.py package/AppxManifest.xml "$out/layout/AppxManifest.xml"
  python3 tools/edge_spoof.py --check "$out/layout/AppxManifest.xml"
else
  cp package/AppxManifest.xml "$out/layout/AppxManifest.xml"
fi
mkdir -p "$out/layout/Assets"
cp tests/out/guest_hello.exe "$out/layout/guest_hello.exe"
if [[ -f "package/branding/boot_anim.tanim" ]]; then
  cp package/branding/boot_anim.tanim "$out/layout/boot_anim.tanim"
fi
# regenerate the real branding assets every pack (sources: package/branding)
python3 tools/gen-assets.py "$out/layout/Assets"
# CONSOLE-PROVEN envelope ONLY: the Xbox AppX deployment is picky about asset
# naming variants — the extra tiles (Square310x310, Wide310x150, per-scale
# Square44x44 subdirs) went in once and the console refused activation with
# 0x80070002. Strip everything the four-asset manifest envelope doesn't name.
find "$out/layout/Assets" -type f ! -name 'StoreLogo.png' \
  ! -name 'Square150x150Logo.png' ! -name 'Square44x44Logo.png' \
  ! -name 'SplashScreen.png' -delete
find "$out/layout/Assets" -type d -empty -delete

# ---- test cert (never committed; subject == Publisher) ----------------------
# Compare the cert subject to the manifest Publisher. OpenSSL prints the RDNs
# in the reverse of the /C=.../CN=... input order; normalize S/ST and spaces.
cert_normalize() {
  local s="$1"
  s="${s#subject=}"
  s="${s#/}"
  s="${s//ST=/S=}"
  s="${s// /}"
  printf '%s' "$s"
}
cert_matches() {
  local prefix="$1" want
  want="$(cert_normalize "$2")"
  [[ -f "$prefix.pem" && -f "$prefix.pfx" && -f "$prefix.cer" ]] || return 1
  local got
  got="$(openssl x509 -in "$prefix.pem" -noout -subject -nameopt RFC2253 2>/dev/null)"
  got="$(cert_normalize "$got")"
  [[ "$got" == "$want" ]]
}

if ! cert_matches "$cert_prefix" "$cert_subj"; then
  if [[ -f "$cert_prefix.pfx" ]]; then
    echo "note: regenerating $cert_label cert (subject changed or missing)"
    rm -f "$cert_prefix.pfx" "$cert_prefix.cer" "$cert_prefix.key" \
          "$cert_prefix.pem" "$cert_prefix-ca.pem"
  fi
  openssl req -x509 -newkey rsa:2048 -nodes \
    -keyout "$cert_prefix.key" -out "$cert_prefix.pem" -days 825 \
    -subj "$cert_subj" \
    -addext "basicConstraints=critical,CA:FALSE" \
    -addext "keyUsage=critical,digitalSignature" \
    -addext "extendedKeyUsage=codeSigning,1.3.6.1.5.5.7.3.3" 2>/dev/null
  openssl pkcs12 -export -out "$cert_prefix.pfx" -inkey "$cert_prefix.key" \
    -in "$cert_prefix.pem" -passout pass:tiramisu
  openssl x509 -in "$cert_prefix.pem" -outform DER -out "$cert_prefix.cer"
fi
[[ -f "$cert_prefix.cer" ]] || { echo "ERROR: $cert_prefix.cer missing" >&2; exit 3; }

# ---- pack + sign -------------------------------------------------------------
rm -f "$out/$pkg_name-unsigned.msix" "$out/$pkg_name.msix"
makemsix pack -d "$out/layout" -p "$out/$pkg_name-unsigned.msix"
osslsigncode sign -pkcs12 "$cert_prefix.pfx" -pass tiramisu -h sha256 \
  -in "$out/$pkg_name-unsigned.msix" -out "$out/$pkg_name.msix"

# ---- verify (fail the build on any doubt) ------------------------------------
openssl x509 -in "$cert_prefix.cer" -inform DER -out "$cert_prefix-ca.pem" -outform PEM
osslsigncode verify -in "$out/$pkg_name.msix" -CAfile "$cert_prefix-ca.pem" \
  | grep -q 'Signature verification: ok' \
  || { echo "FATAL: signature verification FAILED" >&2; exit 4; }
rm -rf /tmp/msix-rt.$$ && makemsix unpack -p "$out/$pkg_name.msix" -d "/tmp/msix-rt.$$" -ac >/dev/null 2>&1 \
  || { echo "FATAL: round-trip unpack FAILED" >&2; exit 4; }
grep -q 'Executable="tiramisu.exe"' "/tmp/msix-rt.$$/AppxManifest.xml" \
  || { echo "FATAL: packed manifest lost the entry point" >&2; exit 4; }
if [[ "$spoof" == "1" ]]; then
  python3 tools/edge_spoof.py --check "/tmp/msix-rt.$$/AppxManifest.xml" \
    || { echo "FATAL: packed manifest lost the Edge identity" >&2; exit 4; }
else
  grep -q 'Name="Xan.Tiramisu"' "/tmp/msix-rt.$$/AppxManifest.xml" \
    || { echo "FATAL: packed manifest lost the public identity" >&2; exit 4; }
  grep -q 'Publisher="CN=TiramisuDev"' "/tmp/msix-rt.$$/AppxManifest.xml" \
    || { echo "FATAL: packed manifest lost the public publisher" >&2; exit 4; }
fi
[[ -f "/tmp/msix-rt.$$/guest_hello.exe" ]] \
  || { echo "FATAL: guest exe missing from package" >&2; exit 4; }
rm -rf "/tmp/msix-rt.$$"

echo "---- done ($cert_label) ----"
ls -l "$out/$pkg_name.msix" "$cert_prefix.cer"
echo
if [[ "$spoof" == "1" ]]; then
  echo "Deploy (own Dev Mode console; KBM / hardware-mouse experiment):"
  echo "  DP_HOST=<console-ip> DP_PASS=<portal-pass> $here/tools/deploy.sh install $out/$pkg_name.msix $cert_prefix.cer"
  echo "  Read XBOX-MOUSE.md first. The Edge PFN has its own LocalState;"
  echo "  migrate existing games/saves with tools/migrate-localstate.py."
else
  echo "Deploy:  DP_HOST=<console-ip> DP_PASS=<portal-pass> $here/tools/deploy.sh install $out/$pkg_name.msix $cert_prefix.cer"
fi
