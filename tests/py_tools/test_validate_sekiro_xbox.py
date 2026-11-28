import copy
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))

import validate_sekiro_xbox as validation

GAME = "Sekiro Shadows Die Twice"
PACKAGE = "Xan.Tiramisu_0.2.5.31_x64__test"
IDLE = {"guest_running": False, "launch_pending": False, "control_code": "PRIVATE"}


class Dashboard:
    def __init__(self):
        self.routes = {
            "/api/status": IDLE,
            "/api/steam/status": {"signed_in": True, "qr_url": "PRIVATE"},
            "/api/steam/library": [{
                "appid": 814380, "account_owned": True, "install_name": GAME, "installed": True,
            }],
            "/games": [{
                "name": GAME, "appid": "814380", "exe": "sekiro.exe",
                "location": "internal", "installed_internal": True, "compat_ok": True,
            }],
        }
        self.routes = copy.deepcopy(self.routes)

    def request(self, route):
        value = self.routes.get(route, {})
        return value.pop(0) if route == "/api/status" and isinstance(value, list) else value


class Portal:
    def __init__(self):
        self.active = PACKAGE
        self.files = {"sekiro.exe": {}, "STEAM_API64.DLL": {}}

    def call(self, method, path, tries):
        assert (method, path, tries) == ("GET", "/api/resourcemanager/processes", 1)
        return 200, json.dumps({"Processes": [{
            "ImageName": "Tiramisu.exe", "PackageFullName": self.active,
        }]}).encode()

    def listing(self, package, path):
        assert (package, path) == (PACKAGE, rf"\LocalState\games\{GAME}")
        return self.files


class Tests(unittest.TestCase):
    def setUp(self):
        self.dash = Dashboard()
        self.portal = Portal()

    def check(self, verified=True):
        return validation.preflight(self.dash, self.portal, PACKAGE, GAME, files_verified=verified)

    def test_allowed_target(self):
        self.assertEqual(self.check(), GAME)

    def test_spacewar_alias_is_rejected(self):
        self.dash.routes["/games"][0]["appid"] = "480"
        with self.assertRaisesRegex(validation.ValidationError, "play_entry_appid_mismatch"):
            self.check()

    def test_missing_account_origin_is_rejected(self):
        del self.dash.routes["/api/steam/library"][0]["account_owned"]
        with self.assertRaisesRegex(validation.ValidationError, "account_ownership_unverified"):
            self.check()

    def test_sideload_account_membership_alone_is_rejected(self):
        with self.assertRaisesRegex(validation.ValidationError, "original_install_unverified"):
            self.check(verified=False)

    def test_wrong_local_folder_is_rejected(self):
        self.dash.routes["/api/steam/library"][0]["install_name"] = "Other Copy"
        with self.assertRaisesRegex(validation.ValidationError, "steam_install_binding_unverified"):
            self.check()

    def test_signed_out_is_rejected(self):
        self.dash.routes["/api/steam/status"]["signed_in"] = False
        with self.assertRaisesRegex(validation.ValidationError, "steam_signed_out"):
            self.check()

    def test_blocked_when_a_guest_is_playing_or_queued(self):
        for key in ("guest_running", "launch_pending"):
            with self.subTest(key=key):
                self.dash.routes["/api/status"] = {**IDLE, key: True}
                with self.assertRaisesRegex(validation.ValidationError, "console_busy_or_unknown"):
                    self.check()

    def test_wrong_active_package_is_rejected(self):
        self.portal.active = "Xan.Tiramisu_0.2.5.30_x64__test"
        with self.assertRaisesRegex(validation.ValidationError, "package_not_active"):
            self.check()

    def test_replacement_markers_are_rejected_case_insensitively(self):
        self.portal.files["STEAM_EMU.ini"] = {}
        with self.assertRaisesRegex(validation.ValidationError, "replacement_marker_present"):
            self.check()

    def test_probe_uses_allowlist_and_numeric_types(self):
        self.dash.routes["/api/render-status"] = {
            "d3d11_width": 800, "host_lit_pixels": "PRIVATE", "message": "PRIVATE",
            "d3d11_height": True,
        }
        self.assertEqual(validation.probe(self.dash), {"render-status": {"d3d11_width": 800}})

    def test_stale_checkpoint_is_ignored_until_current_run(self):
        self.dash.routes["/api/status"] = [
            {**IDLE, "launch_pending": True, "launch_name": GAME, "last_launch_name": GAME},
            {**IDLE, "guest_running": True, "running_game": GAME},
            {**IDLE, "last_launch_name": GAME},
        ]
        checkpoint = {"phase": 4, "outcome": 2, "code": 1}
        self.dash.routes["/api/launch-checkpoint"] = checkpoint
        clock = iter((0, 0, 2, 4))
        events = []
        result = validation.record_progress(
            self.dash, GAME, {"launch-checkpoint": checkpoint},
            lambda event, **sample: events.append((event, sample)), timeout=5, interval=2,
            now=lambda: next(clock), wait=lambda _: None,
        )
        self.assertEqual(result, "guest_no_longer_active")
        self.assertEqual(events[0], ("launch_observed", {"elapsed_s": 2, "guest_active": True}))
        self.assertNotIn("PRIVATE", json.dumps(events))

    def test_does_not_collect_from_another_title(self):
        self.dash.routes["/api/status"] = {**IDLE, "guest_running": True, "running_game": "other"}
        with self.assertRaisesRegex(validation.ValidationError, "another_title_running"):
            validation.record_progress(self.dash, GAME, {}, lambda *_: None, timeout=1, interval=1)


if __name__ == "__main__":
    unittest.main()
