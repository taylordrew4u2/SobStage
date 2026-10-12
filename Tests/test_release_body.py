#!/usr/bin/env python3
"""Tests for Tools/release_body.py, which brings a published release page up to
date with its corrected notes file.

The release workflow never edits a release that already exists, so the fixes
to the v1.13.19 to v1.13.25 notes (checksum link, privacy line, rollback
version) never reached their pages. Tests/data/release-body-v1.13.25.txt is
the body GitHub served for v1.13.25 after those fixes: the old notes followed
by the "What's Changed" list GitHub generated. Replacing that body with the
notes file alone would delete the list, and an edit that does not say which
release is the latest may make GitHub call the edited one the latest, which
is what SobStage's update check reads.

Standard library only, so it runs wherever ctest finds a Python 3.
"""

import contextlib
import io
import json
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(ROOT, "Tools"))

import release_body  # noqa: E402

SHIPPED_BODY = os.path.join(HERE, "data", "release-body-v1.13.25.txt")
NOTES = os.path.join(ROOT, "docs", "release-notes")

GENERATED_V1_13_25 = (
    "## What's Changed\n"
    "* v1.13.25: headphones-first monitoring, Mac buffer-size fixes, feedback "
    "detection on, crash recovery by @taylordrew4u2 in "
    "https://github.com/taylordrew4u2/usbmic/pull/182\n"
    "\n"
    "\n"
    "**Full Changelog**: "
    "https://github.com/taylordrew4u2/usbmic/compare/v1.13.24...v1.13.25")


def read(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read()


class ShippedPage(unittest.TestCase):
    def setUp(self):
        self.shipped = read(SHIPPED_BODY)
        self.notes = read(os.path.join(NOTES, "v1.13.25.md"))

    def test_the_list_github_generated_is_found(self):
        head, generated = release_body.split_generated(self.shipped)
        self.assertEqual(generated, GENERATED_V1_13_25)
        self.assertIn("README.md#download", head)
        self.assertNotIn("What's Changed", head)

    def test_corrected_page_is_the_notes_then_the_same_list(self):
        wanted = release_body.corrected_body(self.notes, self.shipped)
        self.assertFalse(release_body.same_body(self.shipped, wanted))
        self.assertEqual(wanted, self.notes.rstrip("\n") + "\n\n\n" + GENERATED_V1_13_25)
        for stale in ("README.md#download", "v1.13.19 remains available",
                      "sends nothing anywhere"):
            self.assertNotIn(stale, wanted)

    def test_running_it_again_changes_nothing(self):
        wanted = release_body.corrected_body(self.notes, self.shipped)
        self.assertEqual(release_body.corrected_body(self.notes, wanted), wanted)


class GeneratedList(unittest.TestCase):
    def test_no_list(self):
        self.assertEqual(release_body.split_generated("Old notes.\n"), ("Old notes.\n", ""))
        self.assertEqual(release_body.corrected_body("New notes.\n", "Old notes.\n"),
                         "New notes.\n")

    def test_only_a_full_changelog_line(self):
        body = "Old.\n\n**Full Changelog**: https://github.com/o/r/commits/v1.0.0\n"
        self.assertEqual(release_body.corrected_body("New.\n", body),
                         "New.\n\n**Full Changelog**: https://github.com/o/r/commits/v1.0.0\n")

    def test_new_contributors_are_kept(self):
        generated = ("## What's Changed\n"
                     "* Fix by @a in https://github.com/o/r/pull/2\n\n"
                     "## New Contributors\n"
                     "* @a made their first contribution in https://github.com/o/r/pull/2\n\n"
                     "**Full Changelog**: https://github.com/o/r/compare/v1...v2")
        self.assertEqual(release_body.split_generated("Old.\n\n" + generated)[1], generated)

    def test_the_notes_own_lines_are_never_taken_for_the_list(self):
        notes_end = ("- See [the fix](https://github.com/o/r/pull/3).\n"
                     "* Thanks to everyone who tested.\n")
        body = ("Old.\n" + notes_end + "\n## What's Changed\n"
                "* Fix by @a in https://github.com/o/r/pull/3\n\n"
                "**Full Changelog**: https://github.com/o/r/compare/v1...v2")
        head, generated = release_body.split_generated(body)
        self.assertTrue(head.startswith("Old.\n" + notes_end), head)
        self.assertTrue(generated.startswith("## What's Changed"), generated)

    def test_a_full_changelog_mention_inside_the_notes_is_not_a_list(self):
        body = "See the **Full Changelog**: below.\nMore notes.\n"
        self.assertEqual(release_body.split_generated(body)[1], "")

    def test_windows_line_endings(self):
        body = "Old.\r\n\r\n**Full Changelog**: https://github.com/o/r/compare/v1...v2"
        self.assertEqual(release_body.corrected_body("New.\r\n", body),
                         "New.\n\n**Full Changelog**: https://github.com/o/r/compare/v1...v2")
        self.assertTrue(release_body.same_body("A\r\nB\r\n\r\n", "A\nB"))


# A stand-in for `gh api` that serves releases from a JSON file and, like
# GitHub, makes an edited release the latest unless the edit says otherwise.
FAKE_GH = r'''
import json, sys
state_path = sys.argv[1]
args = sys.argv[3:]
with open(state_path, encoding="utf-8") as handle:
    state = json.load(handle)
method = "GET"
if args[0] == "-X":
    method, args = args[1], args[2:]
path = args[0]
prefix = "repos/taylordrew4u2/usbmic/releases"
releases = state["releases"]

def find(key, value):
    for release in releases:
        if str(release[key]) == value:
            return release
    sys.stderr.write("gh: Not Found (HTTP 404)\n")
    sys.exit(1)

if method == "PATCH":
    release = find("id", path[len(prefix) + 1:])
    payload = json.loads(sys.stdin.read())
    state["patches"].append({"tag": release["tag_name"], "payload": payload})
    release["body"] = payload["body"]
    if payload.get("make_latest", "true") == "true":
        state["latest"] = release["tag_name"]
    result = release
elif path == prefix + "/latest":
    result = find("tag_name", state["latest"])
elif path.startswith(prefix + "?"):
    result = releases if path.endswith("page=1") else []
elif path.startswith(prefix + "/tags/"):
    result = find("tag_name", path[len(prefix) + 6:])
else:
    result = find("id", path[len(prefix) + 1:])
with open(state_path, "w", encoding="utf-8") as handle:
    json.dump(state, handle)
json.dump(result, sys.stdout)
'''


class PublishedPages(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.mkdtemp()
        script = os.path.join(self.folder, "gh.py")
        with open(script, "w", encoding="utf-8") as handle:
            handle.write(FAKE_GH)
        self.state_path = os.path.join(self.folder, "state.json")
        self.notes = {tag: read(os.path.join(NOTES, tag + ".md"))
                      for tag in ("v1.13.24", "v1.13.25")}
        shipped = read(SHIPPED_BODY)
        self.write_state({
            "latest": "v1.13.25",
            "patches": [],
            "releases": [
                {"id": 25, "tag_name": "v1.13.25", "draft": False, "body": shipped},
                {"id": 24, "tag_name": "v1.13.24", "draft": False,
                 "body": shipped.replace("v1.13.25", "v1.13.24")},
            ]})
        self.saved_gh = release_body.GH
        release_body.GH = [sys.executable, script, self.state_path]

    def tearDown(self):
        release_body.GH = self.saved_gh
        shutil.rmtree(self.folder)

    def write_state(self, state):
        with open(self.state_path, "w", encoding="utf-8") as handle:
            json.dump(state, handle)

    def state(self):
        with open(self.state_path, encoding="utf-8") as handle:
            return json.load(handle)

    def run_tool(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            status = release_body.main(list(args))
        return status, out.getvalue(), err.getvalue()

    def test_a_dry_run_shows_the_change_and_edits_nothing(self):
        status, out, _ = self.run_tool("--all")
        self.assertEqual(status, 1)
        self.assertIn("-To check a download, see [Download in the README]", out)
        self.assertEqual(self.state()["patches"], [])

    def test_apply_updates_each_page_and_leaves_the_latest_release_alone(self):
        # The latest release first: an edit to an older one after it must not
        # take "latest" away from it.
        status, _, err = self.run_tool("v1.13.25", "v1.13.24", "--apply")
        self.assertEqual(status, 0, err)
        state = self.state()
        self.assertEqual(state["latest"], "v1.13.25")
        self.assertEqual([(p["tag"], p["payload"]["make_latest"]) for p in state["patches"]],
                         [("v1.13.25", "true"), ("v1.13.24", "false")])
        for release in state["releases"]:
            body = release["body"]
            self.assertTrue(body.startswith(self.notes[release["tag_name"]].rstrip("\n")))
            self.assertIn("## What's Changed", body)
            self.assertNotIn("README.md#download", body)

        status, out, _ = self.run_tool("--all")
        self.assertEqual(status, 0, out)

    def test_a_tag_without_notes_is_refused_before_anything_is_sent(self):
        status, _, err = self.run_tool("v0.0.1", "--apply")
        self.assertEqual(status, 2)
        self.assertIn("no notes file", err)
        self.assertEqual(self.state()["patches"], [])


if __name__ == "__main__":
    unittest.main()
