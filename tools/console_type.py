#!/usr/bin/env python3
"""Detect the Xbox generation for a Dev Mode console and apply a matching
Tiramisu tweak.  Works through Device Portal (`/api/os/info`).

Console detection is heuristic: Device Portal is the only public identity
source.  `Platform.Model` is used first; a few fallback keys are accepted
(`ConsoleType`, `ProductId`, `ConsoleModel`) so consoles that expose the model
somewhere else still work.  Unknown models stay honest: the tool reports the
raw model and the caller decides.

Usage:
    tools/console_type.py detect --console <ip> [--json]
    tools/console_type.py detect --console <ip> --apply
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))
from devportal import DevicePortal, DevicePortalError  # noqa: E402

# Well-known ProductIds for what we can still detect cheaply.  Most IDs are
# tied to tools of the OS build rather than a simple string key; keep these as
# fallbacks and document that Platform.Model is the primary source.
PRODUCT_IDS = {
    # Careful: keep aliases human readable; unknown products stay unknown.
}

MODEL_ALIASES = {
    "series_x": "series_x",
    "series x": "series_x",
    "xbox series x": "series_x",
    "xbox series xs": "series_x",
    "xbox series x:s": "series_x",
    "series_s": "series_s",
    "series s": "series_s",
    "xbox series s": "series_s",
    "xbox series ss": "series_s",
    "xbox series s:s": "series_s",
    "one_x": "one_x",
    "one x": "one_x",
    "xbox one x": "one_x",
    "xbox one x|s": "one_x",
    "scorpio": "one_x",
    "project scorpio": "one_x",
    "one_s": "one_s",
    "one s": "one_s",
    "xbox one s": "one_s",
    "xbox one s|x": "one_s",
    "edmonton": "one_s",
    "xbox one s all-digital": "one_s",
    "one": "one",
    "xbox_one": "one",
    "xbox one": "one",
    "durango": "one",
    "xbox one original": "one",
    "xbox one (original)": "one",
    "xbox one (2013)": "one",
    "xbox one fat": "one",
    "xboxone": "one",
}

# Where Tiramisu consumes its settings: see src/host/settings.cpp /
# src/host/webxfer.cpp.  Keep this list short and changes here must be made
# with a hardware touch-up pass on the real console.
TWEAKS = {
    "series_x": {
        "console": "Series X",
        "family": "series_x",
        "description": "full D3D11/D3D12 target, 5 GB headroom. Validate on real hardware.",
        "settings": {"os_build": 22621, "memory_budget_mb": 5120},
        "notes": "No console proof exists yet; Series S is our measured baseline.",
    },
    "series_s": {
        "console": "Series S",
        "family": "series_s",
        "description": "5 GB in Dev Mode; slow parts are the newest FSB5-heap-corruption-prone path.",
        "settings": {"os_build": 22621, "memory_budget_mb": 5120},
        "notes": "Known console-proof titles: RACCOIN, Celeste, God of War.",
    },
    "one_x": {
        "console": "One X",
        "family": "one_x",
        "description": "Xbox One X (Scorpio), 8-core Jaguar Evolved @ 2.3 GHz, GCN 6 TF, 12 GB GDDR5 (326 GB/s); 5 GB Dev Mode headroom.",
        "settings": {
            "os_build": 22621,
            "memory_budget_mb": 5120,
            "resolution_scale": 100,
            "frame_rate_cap": 60,
            "texture_quality": 2,
        },
        "notes": "High-performance last-gen target with 12 GB unified GDDR5; supports 4K native rendering and 5 GB Dev Mode guest budget.",
    },
    "one_s": {
        "console": "One S",
        "family": "one_s",
        "description": "Xbox One S (Edmonton), 8-core Jaguar @ 1.75 GHz, GCN 1.4 TF (HDR), 8 GB DDR3 + 32 MB ESRAM; 5 GB Dev Mode headroom.",
        "settings": {
            "os_build": 19045,
            "memory_budget_mb": 5120,
            "resolution_scale": 85,
            "frame_rate_cap": 60,
            "texture_quality": 1,
        },
        "notes": "Slim last-gen target; 5 GB Dev Mode headroom, D3D11 preferred, 32 MB ESRAM-accelerated buffering.",
    },
    "one": {
        "console": "Xbox One",
        "family": "one",
        "description": "Original 2013 Xbox One (Durango), 8-core Jaguar @ 1.75 GHz, GCN 1.31 TF, 8 GB DDR3 + 32 MB ESRAM; 5 GB Dev Mode headroom.",
        "settings": {
            "os_build": 19045,
            "memory_budget_mb": 5120,
            "resolution_scale": 80,
            "frame_rate_cap": 30,
            "texture_quality": 1,
        },
        "notes": "Baseline last-gen target; 5 GB Dev Mode headroom, conservative 80% scale, 30 FPS cap, 32 MB ESRAM.",
    },
}

DEFAULT_TWEAK = {
    "console": None,
    "family": "unknown",
    "description": "unknown console - no auto settings generated",
    "settings": {},
    "notes": "Set --console host or edit settings.ini manually.",
}


def detect_generation(os_info):
    """Return e.g. 'series_s' or None. Never guesses from nothing."""
    if not isinstance(os_info, dict):
        return None, ""
    platform = os_info.get("Platform")
    platform_model = platform.get("Model") if isinstance(platform, dict) else platform
    model = (platform_model
             or os_info.get("ConsoleType")
             or os_info.get("ProductId")
             or os_info.get("ConsoleModel")
             or os_info.get("OSModel")
             or os_info.get("MachineName"))
    if not model:
        return None, ""
    key = str(model).lower().strip()
    family = MODEL_ALIASES.get(key)
    if family is None:
        for alias, name in MODEL_ALIASES.items():
            if key == alias:
                family = name
                break
    return family, str(model)


def tweak_for(os_info):
    family, raw_model = detect_generation(os_info)
    if not family:
        return family, dict(DEFAULT_TWEAK, console=raw_model or "unknown")
    tweak = json.loads(json.dumps(TWEAKS[family]))  # deep-copy default
    tweak["console"] = raw_model or tweak["console"]
    return family, tweak


def tweak_file_name(tweak):
    fam = tweak.get("family")
    return f"console_tweak_{fam}.json" if fam else "console_tweak.json"


def find_tiramisu_package(portal):
    wanted = [p for p in portal.packages() if p.get("Name") == "Xan.Tiramisu"]
    if len(wanted) == 1:
        return wanted[0]
    if wanted:
        return max(wanted, key=lambda p: tuple(
            int(part) if part.isdigit() else 0 for part in
            (p.get("Version", "").split(".") or ["0"])))
    raise DevicePortalError("no Xan.Tiramisu package found on console")


def apply_tweak(portal, tweak, package_full=None):
    package = find_tiramisu_package(portal) if not package_full else {
        "PackageFullName": package_full}
    portal.ensure_dir(package["PackageFullName"], "\\LocalState")
    name = tweak_file_name(tweak)
    body = json.dumps(tweak, indent=2, sort_keys=True) + "\n"
    with tempfile.TemporaryDirectory(prefix="tiramisu-console-type-") as td:
        local = Path(td) / name
        local.write_text(body, encoding="utf-8")
        portal.put_files(package["PackageFullName"], "\\LocalState", [str(local)])
    print(f"wrote LocalState\\{name} on {tweak.get('console', '?')}")
    print(json.dumps(tweak, indent=2, sort_keys=True))
    return name


def detect(portal, apply=False, package_full=None):
    os_info = portal.connect()
    family, tweak = tweak_for(os_info)
    print(json.dumps(tweak, indent=2, sort_keys=True))
    if apply:
        apply_tweak(portal, tweak, package_full)
    return family, tweak


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("action", choices=["detect"])
    ap.add_argument("--console", default=os.environ.get("XBOX_IP", "10.0.0.23"))
    ap.add_argument("--user", default=os.environ.get("XBOX_USER", ""))
    ap.add_argument("--password", default=os.environ.get("XBOX_PASSWORD", ""))
    ap.add_argument("--port", type=int, default=int(os.environ.get("XBOX_PORT", "11443")))
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--package", default=None, help="PackageFullName to write")
    args = ap.parse_args(argv)
    try:
        portal = DevicePortal(args.console, args.user or None, args.password or None, port=args.port)
        family, _ = detect(portal, apply=args.apply, package_full=args.package)
    except DevicePortalError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0 if family else 2


if __name__ == "__main__":
    raise SystemExit(main())
