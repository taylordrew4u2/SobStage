#!/usr/bin/env python3
"""The newest release notes must not send anyone to a page that is not there.

The release workflow publishes docs/release-notes/<tag>.md as the GitHub
release body, and each version's notes start as a copy of the last one. From
v1.13.19 to v1.13.25 they told Mac users "To check a download, see Download in
the README", linking README.md#download. The README had lost that section, so
the link opened the top of the README and the checksum steps were nowhere in
sight. This checks every link into this repository in the newest notes: the
file must exist, a #section must be a heading in it, and the link must point
at the notes' own tag.

Standard library only, so it runs wherever ctest finds a Python 3.
"""

import os
import re
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
NOTES = os.path.join(ROOT, "docs", "release-notes")

REPOSITORY_LINK = re.compile(
    r"\]\(https://github\.com/taylordrew4u2/usbmic/blob/([^/)\s]+)/([^)#\s]+)(?:#([^)\s]+))?\)")
HEADING = re.compile(r"^(#{1,6})\s+(.*?)\s*#*\s*$")
EXPLICIT_ANCHOR = re.compile(r"<a\s+(?:name|id)=\"([^\"]+)\"")


def slug(heading):
    """The #anchor GitHub gives a Markdown heading."""
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", heading)
    text = text.replace("`", "").replace("*", "").strip().lower()
    text = re.sub(r"[^\w\- ]", "", text)
    return text.replace(" ", "-")


def anchors(markdown):
    """Every #anchor a Markdown file offers, skipping fenced code blocks."""
    found, seen, fenced = set(), {}, False
    for line in markdown.splitlines():
        if line.lstrip().startswith("```"):
            fenced = not fenced
            continue
        if fenced:
            continue
        found.update(EXPLICIT_ANCHOR.findall(line))
        match = HEADING.match(line)
        if match:
            base = slug(match.group(2))
            count = seen.get(base, 0)
            seen[base] = count + 1
            found.add(base if count == 0 else "%s-%d" % (base, count))
    return found


def broken_links(notes_text, tag, root=ROOT):
    """Each link into this repository in NOTES_TEXT that does not lead anywhere."""
    problems = []
    for ref, path, anchor in REPOSITORY_LINK.findall(notes_text):
        target = os.path.join(root, *path.split("/"))
        link = path + ("#" + anchor if anchor else "")
        if ref != tag:
            problems.append("%s points at %s, not %s" % (link, ref, tag))
        if not os.path.isfile(target):
            problems.append("%s: no such file" % link)
        elif anchor:
            with open(target, encoding="utf-8") as handle:
                if anchor not in anchors(handle.read()):
                    problems.append("%s: %s has no such section" % (link, path))
    return problems


def newest_notes():
    versions = []
    for name in os.listdir(NOTES):
        match = re.match(r"^v(\d+)\.(\d+)\.(\d+)\.md$", name)
        if match:
            versions.append((tuple(int(part) for part in match.groups()), name))
    return max(versions)[1]


class Slugs(unittest.TestCase):
    def test_headings_become_github_anchors(self):
        self.assertEqual(slug("Download"), "download")
        self.assertEqual(slug("macOS"), "macos")
        self.assertEqual(slug("Troubleshooting (macOS)"), "troubleshooting-macos")
        self.assertEqual(slug("What to expect on your platform"),
                         "what-to-expect-on-your-platform")
        self.assertEqual(slug("Installing → macOS"), "installing--macos")
        self.assertEqual(slug("`session.json` [fields](x.md)"), "sessionjson-fields")

    def test_code_blocks_and_repeats(self):
        text = "## Download\n```sh\n# macOS\n```\n## Notes\n## Notes\n<a name=\"top\"></a>\n"
        self.assertEqual(anchors(text), {"download", "notes", "notes-1", "top"})


class NewestNotes(unittest.TestCase):
    def test_every_repository_link_leads_somewhere(self):
        name = newest_notes()
        with open(os.path.join(NOTES, name), encoding="utf-8") as handle:
            text = handle.read()
        self.assertTrue(REPOSITORY_LINK.search(text), name)
        self.assertEqual(broken_links(text, name[:-3]), [], name)

    def test_checksum_steps_are_where_the_notes_point(self):
        with open(os.path.join(ROOT, "docs", "INSTALLING.md"), encoding="utf-8") as handle:
            installing = handle.read()
        self.assertIn("download", anchors(installing))
        self.assertIn("grep ' SobStage-macOS.dmg$' SHA256SUMS | shasum -a 256 -c -", installing)

    def test_the_v1_13_25_download_link_is_caught(self):
        shipped = ("To check a download, see [Download in the README]"
                   "(https://github.com/taylordrew4u2/usbmic/blob/v1.13.25/README.md#download).")
        self.assertEqual(broken_links(shipped, "v1.13.25"),
                         ["README.md#download: README.md has no such section"])

    def test_a_link_left_on_the_previous_tag_is_caught(self):
        copied = "[`PRIVACY.md`](https://github.com/taylordrew4u2/usbmic/blob/v1.13.24/PRIVACY.md)"
        self.assertEqual(broken_links(copied, "v1.13.25"),
                         ["PRIVACY.md points at v1.13.24, not v1.13.25"])


if __name__ == "__main__":
    unittest.main()
