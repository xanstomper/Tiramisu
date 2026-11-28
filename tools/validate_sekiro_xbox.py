#!/usr/bin/env python3
"""Validate one Sekiro Play request through the Xbox Dev Mode interfaces."""

import argparse
import fcntl
import json
import math
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

from devportal import DevicePortal, DevicePortalError

APP_ID = 814380
LOCK_PATH = "/tmp/tiramisu-xbox-play-validation.lock"
BODY_LIMIT = 8 * 1024 * 1024
KNOWN_REPLACEMENTS = {
    "steam_emu.ini", "steam_api64.cdx", "sekiro.cdx", "steam_settings",
    "cream_api.ini", "creamapi.ini", "codex64.dll", "goldberg_emu.json",
    "steamclient_loader.exe",
}
PROBE_FIELDS = {
    "/api/launch-checkpoint": ("phase", "outcome", "code"),
    "/api/launch-worker-fault": ("phase", "code"),
    "/api/render-status": (
        "d3d11_device_calls", "d3d11_swapchains", "d3d11_present_calls",
        "d3d11_successful_presents", "d3d11_last_present_result",
        "d3d11_width", "d3d11_height", "guest_lit_pixels", "host_lit_pixels",
        "surface_samples", "surface_probe_result", "guest_commit_mib",
        "guest_commit_budget_mib",
    ),
}


class ValidationError(RuntimeError):
    """A fixed, reportable validation failure free of response or account data."""


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


class Dashboard:
    def __init__(self, host, port):
        address = f"[{host}]" if ":" in host else host
        self.base = f"http://{address}:{port}"
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())

    def request(self, route, *, post=False):
        request = urllib.request.Request(self.base + route, data=b"" if post else None)
        try:
            with self.opener.open(request, timeout=5) as response:
                if response.status != (202 if post else 200):
                    raise ValidationError("unexpected_dashboard_status")
                data = response.read(BODY_LIMIT + 1)
            if len(data) > BODY_LIMIT:
                raise ValidationError("oversized_dashboard_response")
            return json.loads(data)
        except (OSError, ValueError) as exc:
            if isinstance(exc, urllib.error.HTTPError) and exc.code == 409:
                raise ValidationError("launch_conflict") from None
            raise ValidationError("dashboard_request_failed") from None


def idle(dashboard):
    state = dashboard.request("/api/status")
    if (not isinstance(state, dict) or state.get("guest_running") is not False or
            state.get("launch_pending") is not False or state.get("running_game")):
        raise ValidationError("console_busy_or_unknown")


def active_package(portal, expected):
    code, data = portal.call("GET", "/api/resourcemanager/processes", tries=1)
    if code != 200:
        raise ValidationError("active_package_unavailable")
    try:
        processes = json.loads(data)["Processes"]
        active = {
            row["PackageFullName"] for row in processes
            if row.get("ImageName", "").casefold() == "tiramisu.exe"
        }
    except (AttributeError, KeyError, TypeError, ValueError):
        raise ValidationError("active_package_unavailable") from None
    if active != {expected}:
        raise ValidationError("package_not_active")


def rows(dashboard, route):
    result = dashboard.request(route)
    if not isinstance(result, list) or not all(isinstance(row, dict) for row in result):
        raise ValidationError("invalid_library_response")
    return result


def app_id_matches(value):
    return ((type(value) is int and value == APP_ID) or
            (isinstance(value, str) and value == str(APP_ID)))


def safe_game_name(value):
    return (isinstance(value, str) and 0 < len(value.encode("utf-8")) <= 160 and
            value == value.strip() and not value.startswith(".") and
            all(ord(c) >= 32 and c not in '<>:/\\|?*"\x7f' for c in value))


def preflight(dashboard, portal, package, game, *, files_verified):
    idle(dashboard)
    active_package(portal, package)
    if not safe_game_name(game):
        raise ValidationError("invalid_game_name")
    entries = [row for row in rows(dashboard, "/games")
               if isinstance(row.get("name"), str) and row["name"].casefold() == game.casefold()]
    if len(entries) != 1 or entries[0]["name"] != game:
        raise ValidationError("play_entry_unverified")
    entry = entries[0]
    if (entry.get("location") != "internal" or entry.get("installed_internal") is not True or
            entry.get("compat_ok") is not True or
            not isinstance(entry.get("exe"), str) or entry["exe"].casefold() != "sekiro.exe"):
        raise ValidationError("play_entry_unverified")
    parts = r"\LocalState\games" + "\\" + game
    listing = portal.listing(package, parts)
    if not isinstance(listing, dict) or not listing:
        raise ValidationError("install_listing_unavailable")
    names = {file.casefold() for file in listing}
    if names & KNOWN_REPLACEMENTS:
        raise ValidationError("replacement_marker_present")
    if "sekiro.exe" not in names or "steam_api64.dll" not in names:
        raise ValidationError("original_executable_or_sdk_unavailable")
    if not app_id_matches(entry.get("appid")):
        raise ValidationError("play_entry_appid_mismatch")

    account = dashboard.request("/api/steam/status")
    if not isinstance(account, dict) or account.get("signed_in") is not True:
        raise ValidationError("steam_signed_out")
    steam = [row for row in rows(dashboard, "/api/steam/library")
             if app_id_matches(row.get("appid"))]
    if len(steam) != 1 or steam[0].get("account_owned") is not True:
        raise ValidationError("account_ownership_unverified")
    item = steam[0]
    if item.get("installed") is not True or item.get("downloading") or item.get("queued"):
        raise ValidationError("steam_install_unready")
    if item.get("install_name") != game:
        raise ValidationError("steam_install_binding_unverified")

    # API and folder metadata bind the title and catch obvious tampering. The
    # verifier must independently establish the provenance of the installed files.
    if not files_verified:
        raise ValidationError("original_install_unverified")
    return game


def probe(dashboard):
    observations = {}
    for route, fields in PROBE_FIELDS.items():
        try:
            value = dashboard.request(route)
        except ValidationError:
            continue
        if isinstance(value, dict) and value.get("available", True) is True:
            sample = {key: value[key] for key in fields if type(value.get(key)) is int}
            if sample:
                observations[route.rsplit("/", 1)[1]] = sample
    return observations


def target_active(state, game):
    return (isinstance(state, dict) and state.get("guest_running") is True and
            state.get("running_game") == game)


def record_progress(dashboard, game, baseline, record, *, timeout, interval,
                    now=time.monotonic, wait=time.sleep):
    started = now()
    seen_active = False
    first_report = True
    previous_render = None
    while True:
        elapsed = now() - started
        # A stale launch report can remain visible during the next launch.
        try:
            state = dashboard.request("/api/status")
        except ValidationError:
            record("sample", elapsed_s=round(elapsed, 3), status="telemetry_unavailable")
            state = None
        if state is not None and not isinstance(state, dict):
            raise ValidationError("invalid_status_response")
        if state is not None:
            active = target_active(state, game)
            if state.get("guest_running") is True and not active:
                raise ValidationError("another_title_running")
            if state.get("launch_pending") is True and state.get("launch_name") != game:
                raise ValidationError("another_title_queued")
            data = probe(dashboard)
            # Only attach diagnostic details to this run after observing a live
            # target or a changed durable checkpoint when Play attributed it.
            checkpoint = data.get("launch-checkpoint")
            changed = checkpoint is not None and checkpoint != baseline.get("launch-checkpoint")
            attributed = state.get("last_launch_name") == game
            if active or seen_active or (attributed and changed):
                seen_active = seen_active or active
                # Some builds publish the guest name before resetting counters.
                # Keep the initial checkpoint and worker-fault baseline out.
                data = {key: value for key, value in data.items()
                        if value != baseline.get(key)}
                if first_report:
                    record("launch_observed", elapsed_s=round(elapsed, 3), guest_active=active)
                    first_report = False
                render = data.get("render-status")
                sample = {"elapsed_s": round(elapsed, 3), "guest_active": active,
                          "metrics": data}
                if active and render and previous_render:
                    previous_time, previous_count = previous_render
                    counter = render.get("d3d11_successful_presents")
                    if counter is not None and elapsed > previous_time and counter >= previous_count:
                        sample["successful_presents_per_sec"] = round(
                            (counter - previous_count) / (elapsed - previous_time), 2)
                if active and render and "d3d11_successful_presents" in render:
                    previous_render = (elapsed, render["d3d11_successful_presents"])
                record("sample", **sample)
                if not active and state.get("launch_pending") is False:
                    return "guest_no_longer_active"
        if elapsed >= timeout:
            return "needs_visual_observation" if seen_active else "launch_unverified"
        wait(min(interval, timeout - elapsed))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, help="Console hostname or IP on a trusted LAN")
    parser.add_argument("--dashboard-port", type=int, default=9127)
    parser.add_argument("--device-portal-port", type=int, default=11443)
    parser.add_argument("--package", required=True, help="Exact active Tiramisu package name")
    parser.add_argument("--game", required=True, help="Exact installed Sekiro folder name")
    parser.add_argument("--launch", action="store_true", help="Submit a single normal Play request")
    parser.add_argument("--verified-original-install", action="store_true",
                        help="Attest that this exact install was independently verified against original files")
    parser.add_argument("--seconds", type=float, default=360)
    parser.add_argument("--interval", type=float, default=5)
    parser.add_argument("--report", required=True, help="New JSONL report file created with mode 0600")
    args = parser.parse_args()

    if (not re.fullmatch(r"[a-zA-Z0-9.:\-]+", args.host) or
            not re.fullmatch(r"Xan\.Tiramisu_\d+(?:\.\d+){3}_x64__[a-z0-9]+", args.package) or
            not safe_game_name(args.game) or
            not (math.isfinite(args.seconds) and 1 <= args.seconds <= 3600) or
            not (math.isfinite(args.interval) and 1 <= args.interval <= 60) or
            not 1 <= args.dashboard_port <= 65535 or not 1 <= args.device_portal_port <= 65535):
        parser.error("invalid host, exact package name, title, port, or polling interval")

    try:
        fd = os.open(args.report, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    except OSError:
        print("report_create_failed", file=sys.stderr)
        return 2
    with os.fdopen(fd, "w", encoding="utf-8") as output:
        def record(event, **values):
            output.write(json.dumps({"event": event, **values}, ensure_ascii=True) + "\n")
            output.flush()

        record("validation_started", appid=APP_ID, package=args.package,
               test_mode="launch" if args.launch else "preflight")
        dashboard = Dashboard(args.host, args.dashboard_port)
        portal = DevicePortal(args.host, user=os.environ.get("DP_USER"),
                              password=os.environ.get("DP_PASS"), port=args.device_portal_port,
                              timeout=5, log=lambda *_: None)
        try:
            with open(LOCK_PATH, "a") as lock:
                try:
                    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                except BlockingIOError:
                    raise ValidationError("console_validation_locked") from None
                portal.connect()
                game = preflight(dashboard, portal, args.package, args.game,
                                 files_verified=args.verified_original_install)
                record("preflight_passed")
                if not args.launch:
                    return 0
                baseline = probe(dashboard)
                idle(dashboard)
                queued = dashboard.request("/api/launch/" + urllib.parse.quote(game, safe=""), post=True)
                if not isinstance(queued, dict) or queued.get("queued") is not True or queued.get("name") != game:
                    raise ValidationError("unexpected_launch_acknowledgement")
                record("play_queued")
                result = record_progress(dashboard, game, baseline, record,
                                         timeout=args.seconds, interval=args.interval)
                record("validation_finished", result=result)
                return 3 if result != "needs_visual_observation" else 0
        except ValidationError as exc:
            record("validation_blocked", reason=str(exc))
            print(str(exc), file=sys.stderr)
        except (DevicePortalError, OSError, ValueError):
            record("validation_blocked", reason="portal_or_workspace_unavailable")
            print("portal_or_workspace_unavailable", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
