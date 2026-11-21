#!/usr/bin/env python3
"""Tests for the pure parts of tools/ci/release_archive.py (the Releases-page entry plan). No network.
Run: python3 tests/release_archive_test.py"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "ci"))
import release_archive as ra  # noqa: E402

ENTRY = {"version": "1.2.0", "date": "2026-11-01", "notes": ["Fixed a thing", "Added a thing"],
         "downloads": {"windows": {"file": "skullburglar-1.2.0-windows-x86_64-setup.exe"},
                       "macos": {"file": "skullburglar-1.2.0-macos-arm64.dmg"},
                       "linux_appimage": {"file": "skullburglar-1.2.0-linux-x86_64.AppImage"},
                       "linux_tar": {"file": "skullburglar-1.2.0-linux-x86_64.tar.xz"}}}


class Plan(unittest.TestCase):
    def test_description(self):
        d = ra.release_description(ENTRY)
        self.assertIn("Released 2026-11-01.", d)
        self.assertIn("- Fixed a thing\n- Added a thing", d)
        self.assertIn("No notes recorded", ra.release_description({"version": "1"}))

    def test_links(self):
        links = ra.release_links(ENTRY)
        self.assertEqual([lk["name"].split(":")[0] for lk in links],
                         ["Windows installer", "macOS", "Linux AppImage", "Linux .tar.xz"])
        self.assertEqual(links[1]["url"], ra.archive_url("1.2.0", "skullburglar-1.2.0-macos-arm64.dmg"))
        self.assertTrue(all(lk["link_type"] == "package" for lk in links))

    def test_create(self):
        kind, body = ra.release_plan(ENTRY)
        self.assertEqual(kind, "create")
        self.assertEqual(body["tag_name"], "v1.2.0")
        self.assertEqual(body["released_at"], "2026-11-01T12:00:00Z")
        self.assertEqual(len(body["assets"]["links"]), 4)

    def test_update_adds_only_missing_links(self):
        have = [ra.archive_url("1.2.0", "skullburglar-1.2.0-macos-arm64.dmg")]
        kind, body, missing = ra.release_plan(ENTRY, have)
        self.assertEqual(kind, "update")
        self.assertNotIn("tag_name", body)
        self.assertEqual(len(missing), 3)
        self.assertEqual(ra.release_plan(ENTRY, [lk["url"] for lk in ra.release_links(ENTRY)])[2], [])


if __name__ == "__main__":
    unittest.main()
