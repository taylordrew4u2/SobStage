#!/usr/bin/env python3
"""Bring a published GitHub release page up to date with its notes file.

The release workflow publishes docs/release-notes/<tag>.md as the release body
once, when it creates the release, and refuses to touch a release that already
exists. A later fix to a notes file therefore never reaches the release page.
After the notes for v1.13.19 to v1.13.25 were corrected, their pages still sent
Mac users to a README section that no longer existed to check a download, and
still said "SobStage sends nothing anywhere".

GitHub appends its own list to each body ("## What's Changed", "## New
Contributors", "**Full Changelog**: ..."). Replacing the body with the notes
file alone would delete that list, so this keeps it.

  release_body.py TAG [TAG...]   show what would change on those release pages
  release_body.py --all          the same for every published release that has
                                 a notes file
  ... --apply                    make the change (needs the GitHub CLI, `gh`,
                                 signed in with write access to the repository)

Only the body is sent: assets, tag, title and pre-release state stay as they
are. SobStage's update check reads which release is "latest", and GitHub may
mark an edited release as latest unless told otherwise, so every update says
explicitly whether its release is the latest, and the latest release is
checked again afterwards.

Exit status: 0 when every page matches its notes (with --apply, once every
update has landed), 1 when a page differs (without --apply), 2 on an error.

Standard library only, so it runs on macOS's stock python3 (3.8 or later).
"""

import argparse
import difflib
import json
import os
import re
import subprocess
import sys

REPOSITORY = "taylordrew4u2/usbmic"
GH = ["gh"]  # the GitHub CLI; the tests put a stand-in here
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NOTES = os.path.join(ROOT, "docs", "release-notes")

# The lines of GitHub's generated release notes, in its default layout (this
# repository has no .github/release.yml to change it).
_GENERATED_LINE = re.compile(
    r"^(?:## What's Changed"
    r"|## New Contributors"
    r"|\* .* in https://github\.com/[^/\s]+/[^/\s]+/pull/\d+"
    r"|\*\*Full Changelog\*\*: https://github\.com/\S+)$")
_FULL_CHANGELOG = "**Full Changelog**: "


class ReleaseError(Exception):
    pass


def split_generated(body):
    """BODY as (notes, generated): the text before the list GitHub appended to
    it, and that list. GENERATED is empty when there is no list.

    GitHub's list always ends with its "**Full Changelog**" line. From there
    this walks back over GitHub's own headings and pull-request lines only, so
    nothing the notes say is ever taken for part of the list."""
    lines = body.replace("\r\n", "\n").split("\n")
    end = len(lines)
    while end and not lines[end - 1].strip():
        end -= 1
    if (not end or not lines[end - 1].startswith(_FULL_CHANGELOG)
            or not _GENERATED_LINE.match(lines[end - 1])):
        return "\n".join(lines), ""
    start = end - 1
    for index in range(end - 2, -1, -1):
        if not lines[index].strip():
            continue
        if not _GENERATED_LINE.match(lines[index]):
            break
        start = index
    return "\n".join(lines[:start]), "\n".join(lines[start:])


def corrected_body(notes, published):
    """The release body for NOTES, keeping the list GitHub generated at the end
    of the PUBLISHED body, with the same gap before it."""
    head, generated = split_generated(published)
    notes = notes.replace("\r\n", "\n").rstrip("\n")
    if not generated:
        return notes + "\n"
    blank_lines = len(head) - len(head.rstrip("\n"))
    return notes + "\n" * max(blank_lines + 1, 2) + generated


def same_body(left, right):
    """True when two bodies show the same page (line endings and trailing
    blank lines aside)."""
    return (left.replace("\r\n", "\n").rstrip()
            == right.replace("\r\n", "\n").rstrip())


def _gh(*args, payload=None):
    command = GH + ["api"] + list(args)
    try:
        result = subprocess.run(
            command, input=None if payload is None else json.dumps(payload),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            universal_newlines=True, check=False)
    except FileNotFoundError:
        raise ReleaseError("the GitHub CLI (gh) is not installed")
    if result.returncode != 0:
        raise ReleaseError("%s failed: %s" % (" ".join(command),
                                              result.stderr.strip()))
    return json.loads(result.stdout) if result.stdout.strip() else None


def _latest_tag():
    return _gh("repos/%s/releases/latest" % REPOSITORY)["tag_name"]


def _published_releases():
    releases, page = [], 1
    while True:
        batch = _gh("repos/%s/releases?per_page=100&page=%d" % (REPOSITORY, page))
        if not batch:
            return releases
        releases.extend(batch)
        page += 1


def _notes_path(tag):
    return os.path.join(NOTES, tag + ".md")


def _read_notes(tag):
    with open(_notes_path(tag), encoding="utf-8") as handle:
        return handle.read()


def _update(release, body, latest):
    tag = release["tag_name"]
    _gh("-X", "PATCH", "repos/%s/releases/%d" % (REPOSITORY, release["id"]),
        "-H", "Content-Type: application/json", "--input", "-",
        payload={"body": body,
                 "make_latest": "true" if tag == latest else "false"})
    stored = _gh("repos/%s/releases/%d" % (REPOSITORY, release["id"]))
    if not same_body(stored.get("body") or "", body):
        raise ReleaseError("%s: GitHub did not keep the new release body" % tag)


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Bring published GitHub release pages up to date with "
                    "docs/release-notes/<tag>.md, keeping GitHub's generated "
                    "list of changes.")
    parser.add_argument("tags", nargs="*", metavar="TAG")
    parser.add_argument("--all", action="store_true",
                        help="every published release that has a notes file")
    parser.add_argument("--apply", action="store_true",
                        help="update the release pages (default: only show "
                             "what would change)")
    args = parser.parse_args(argv)
    if bool(args.tags) == args.all:
        parser.error("name the release tags, or pass --all")

    try:
        for tag in args.tags:
            if not os.path.isfile(_notes_path(tag)):
                raise ReleaseError("no notes file docs/release-notes/%s.md" % tag)
        latest = _latest_tag()
        if args.all:
            releases = [release for release in _published_releases()
                        if not release.get("draft")
                        and os.path.isfile(_notes_path(release["tag_name"]))]
        else:
            releases = [_gh("repos/%s/releases/tags/%s" % (REPOSITORY, tag))
                        for tag in args.tags]

        differing = 0
        for release in releases:
            tag = release["tag_name"]
            published = release.get("body") or ""
            wanted = corrected_body(_read_notes(tag), published)
            if same_body(published, wanted):
                print("%s: the release page matches docs/release-notes/%s.md"
                      % (tag, tag))
                continue
            differing += 1
            for line in difflib.unified_diff(
                    published.replace("\r\n", "\n").splitlines(),
                    wanted.splitlines(),
                    "%s release page" % tag, "docs/release-notes/%s.md" % tag,
                    lineterm=""):
                print(line)
            print()
            if args.apply:
                _update(release, wanted, latest)
                print("%s: release page updated" % tag)

        if args.apply and differing:
            now_latest = _latest_tag()
            if now_latest != latest:
                raise ReleaseError(
                    "the latest release moved from %s to %s; set it back on "
                    "GitHub (Edit release -> Set as the latest release)"
                    % (latest, now_latest))
    except ReleaseError as error:
        print("release_body.py: %s" % error, file=sys.stderr)
        return 2

    if differing and not args.apply:
        print("%d release page(s) differ from their notes. Check the changes "
              "above, then run again with --apply." % differing)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
