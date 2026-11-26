#!/usr/bin/env bash
# deploy.sh — Xbox Device Portal client (single script, no dependencies beyond curl/python3)
#
# Env (all overridable):
#   DP_HOST   console IP                        (default 192.168.15.96 — SET YOURS)
#   DP_USER   portal username                   (default: dev set in Dev Home)
#   DP_PASS   portal password                   (required; never printed)
#   DP_PORT   portal HTTPS port                 (default 11443 — Xbox standard)
#
# Commands:
#   install PKG.msix [CERT.cer]   upload + install, poll to completion
#   launch FAMILY!App FULLNAME    activate (rid + package full name)
#   kill FULLNAME                 force stop
#   state                         install-state poll (last result)
#   ps                            running app list
#   info                          OS info (auth sanity check)
#   screenshot OUT.png            grab the TV
#   dumps [FULLNAME]              list user-mode crash dumps (ETW API is 404 on Xbox)
#   uninstall FULLNAME
#   info-file FULLNAME REMOTEPATH  stat a file in the package's sandbox
#
# Endpoint behavior verified against a real Series S by jobucaldas/xboxnative:
#   - CSRF token required, taken from the login cookie jar
#   - taskmanager params are base64-encoded
#   - /api/etw/* does NOT exist on Xbox Dev Mode; use dumps + app log files
set -euo pipefail
DP_HOST="${DP_HOST:?set DP_HOST=<console ip>}"
DP_USER="${DP_USER:-jewboy420}"
DP_PASS="${DP_PASS:?set DP_PASS=<device portal password>}"
DP_PORT="${DP_PORT:-11443}"
BASE="https://$DP_HOST:$DP_PORT"
JAR="$(mktemp)"; trap 'rm -f "$JAR"' EXIT

urlenc() { python3 -c 'import urllib.parse,sys;print(urllib.parse.quote(sys.argv[1],safe=""))' "$1"; }
b64()    { printf %s "$1" | base64 -w0; }

# curl_dp PATH [curl args...] — authenticated; CSRF header from cookie jar
curl_dp() {
  local p="$1"; shift
  if [[ ! -s "$JAR" ]]; then
    curl -sk -m 30 -c "$JAR" -o /dev/null -u "$DP_USER:$DP_PASS" "$BASE/api/os/info" \
      || { echo "ERROR: cannot reach $BASE (console on? dev mode on? IP right?)" >&2; exit 2; }
  fi
  local csrf; csrf=$(awk '$6=="CSRF-Token"{print $7}' "$JAR" | tail -1)
  curl -sk -m "${DP_TIMEOUT:-120}" -b "$JAR" -u "$DP_USER:$DP_PASS" \
    -H "X-CSRF-Token: $csrf" "$@" "$BASE$p"
}

cmd="${1:-}"; shift || true
case "$cmd" in
  info)
    curl_dp "/api/os/info" -w '\nHTTP %{http_code}\n' ;;
  ps)
    curl_dp "/api/taskmanager/list" -w '\nHTTP %{http_code}\n' ;;
  state)
    curl_dp "/api/app/packagemanager/state" -w '\nHTTP %{http_code}\n' ;;
  install)
    pkg="$1"; cer="${2:-}"
    [[ -f "$pkg" ]] || { echo "usage: deploy.sh install PKG.msix [CERT.cer]" >&2; exit 2; }
    args=(-F "$(basename "$pkg")=@$pkg")
    [[ -n "$cer" ]] && args+=(-F "$(basename "$cer")=@$cer")
    curl_dp "/api/app/packagemanager/package?package=$(basename "$pkg")" \
      -X POST "${args[@]}" -o /tmp/dp-install.$$ -w 'upload HTTP %{http_code}\n'
    head -c 400 /tmp/dp-install.$$ 2>/dev/null; echo; rm -f /tmp/dp-install.$$
    for i in $(seq 1 60); do
      s=$(curl_dp "/api/app/packagemanager/state")
      echo "state: $s"
      case "$s" in
        *'"Code" : 0'*|*'"Code": 0'*) echo "INSTALL OK"; exit 0 ;;
        *'"Code" : 80'*|*'"Code" : -'*) echo "INSTALL FAILED"; exit 1 ;;
      esac
      sleep 3
    done
    echo "INSTALL TIMEOUT"; exit 1 ;;
  launch)
    rid="$1"; full="$2"
    curl_dp "/api/taskmanager/app?appid=$(b64 "$rid")&package=$(b64 "$full")" \
      -X POST -H 'Content-Length: 0' -w '\nlaunch HTTP %{http_code}\n'
    echo "(now: deploy.sh screenshot out.png — LOOK at the TV)" ;;
  kill)
    curl_dp "/api/taskmanager/app?package=$(b64 "$1")&forcestop=yes" -X DELETE -w '\nkill HTTP %{http_code}\n' ;;
  uninstall)
    curl_dp "/api/app/packagemanager/package?package=$(urlenc "$1")" -X DELETE -w '\nHTTP %{http_code}\n' ;;
  screenshot)
    curl_dp "/ext/screenshot" -f -o "${1:-shot.png}" -w "\nshot HTTP %{http_code} bytes=%{size_download}\n" ;;
  dumps)
    pkg="${1:-}"
    q=""; [[ -n "$pkg" ]] && q="?packageFullName=$(urlenc "$pkg")"
    curl_dp "/api/debug/dump/usermode/dumps$q" -w '\nHTTP %{http_code}\n' ;;
  info-file)
    pkg="$1"; rp="$2"
    curl_dp "/api/app/packagemanager/info?package=$(urlenc "$pkg")" -w '\nHTTP %{http_code}\n' ;;
  *)
    grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
