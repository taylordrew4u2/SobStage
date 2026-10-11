#!/usr/bin/env python3
"""Tests for Tools/dmg_layout.py, the macOS disk-image window layout.

v1.13.25 shipped a disk image whose window put SUPPORT.md and
RELEASE_CHECKLIST.md on top of the xattr command the background exists to
show, and PRIVACY.md on the title. Tests/data/dmg-v1.13.25.dsstore is the
.DS_Store from that published image: the checks must fail on it and pass on
the layout the release workflow now builds.

Standard library only, so it runs wherever ctest finds a Python 3.
"""

import contextlib
import io
import os
import plistlib
import shutil
import struct
import sys
import tempfile
import unittest
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "Tools"))

import dmg_layout  # noqa: E402

SHIPPED_STORE = os.path.join(HERE, "data", "dmg-v1.13.25.dsstore")
BACKGROUND = os.path.join(ROOT, "Resources", "DmgBackground.png")
BACKGROUND_2X = os.path.join(ROOT, "Resources", "DmgBackground@2x.png")

ALL_DOCUMENTS = dmg_layout.TOP_LEVEL_DOCUMENTS + dmg_layout.FOLDER_DOCUMENTS


# --- a minimal .DS_Store writer, to build images the reader has not seen -----

def _record(name, code, kind, value):
    encoded = name.encode("utf-16-be")
    out = struct.pack(">I", len(encoded) // 2) + encoded + code.encode() + kind.encode()
    if kind == "blob":
        return out + struct.pack(">I", len(value)) + value
    return out + value


def _node(rightmost, entries):
    body = b"".join((struct.pack(">I", child) if rightmost else b"") + record
                    for child, record in entries)
    return struct.pack(">II", rightmost, len(entries)) + body


def write_ds_store(records, split=None):
    """A Bud1 file holding RECORDS, a list of (name, code, kind, value).

    With SPLIT, the tree has an internal root with two leaves, so the reader's
    walk through child pointers is exercised too.
    """
    records = sorted(records, key=lambda item: (item[0].lower(), item[1]))
    encoded = [_record(*item) for item in records]
    # Block 0: allocator info at 2048 (size 2048); block 1: DSDB at 64 (32);
    # blocks 2..: B-tree nodes, 4096 bytes each, from 4096 on.
    if split is None:
        nodes = [_node(0, [(0, record) for record in encoded])]
        root_node = 2
    else:
        left = _node(0, [(0, record) for record in encoded[:split]])
        right = _node(0, [(0, record) for record in encoded[split + 1:]])
        root = _node(4, [(3, encoded[split])])
        nodes = [root, left, right]
        root_node = 2
    addresses = [2048 | 11, 64 | 5] + [(4096 * (index + 1)) | 12 for index in range(len(nodes))]
    allocator = struct.pack(">II", len(addresses), 0)
    allocator += struct.pack(">%dI" % 256, *(addresses + [0] * (256 - len(addresses))))
    allocator += struct.pack(">I", 1) + bytes([4]) + b"DSDB" + struct.pack(">I", 1)
    allocator += struct.pack(">I", 0) * 32
    dsdb = struct.pack(">IIIII", root_node, 0 if split is None else 1, len(records),
                       len(nodes), 0x1000)
    body = bytearray(4096 * (len(nodes) + 1))
    body[64:64 + len(dsdb)] = dsdb
    body[2048:2048 + len(allocator)] = allocator
    for index, node in enumerate(nodes):
        start = 4096 * (index + 1)
        body[start:start + len(node)] = node
    header = b"\x00\x00\x00\x01Bud1" + struct.pack(">III", 2048, 2048, 2048) + bytes(16)
    return header + bytes(body[32:])


def iloc(x, y):
    return struct.pack(">ii", x, y) + b"\xff\xff\xff\xff\xff\xff\x00\x00"


def icon_view(icon_size):
    return plistlib.dumps({"iconSize": float(icon_size), "arrangeBy": "none"},
                          fmt=plistlib.FMT_BINARY)


# --- a minimal PNG reader, so the background can be checked without PIL ------

def read_png(path):
    with open(path, "rb") as handle:
        data = handle.read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", path
    p, idat = 8, b""
    while p < len(data):
        (length,) = struct.unpack_from(">I", data, p)
        kind, body = data[p + 4:p + 8], data[p + 8:p + 8 + length]
        p += 12 + length
        if kind == b"IHDR":
            width, height, depth, colour, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
    assert depth == 8 and colour in (2, 6) and interlace == 0, path
    channels = 3 if colour == 2 else 4
    raw = zlib.decompress(idat)
    stride = width * channels
    rows, previous, p = [], bytearray(stride), 0
    for _ in range(height):
        kind, line = raw[p], bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        for i in range(stride):
            left = line[i - channels] if i >= channels else 0
            up = previous[i]
            if kind == 1:
                line[i] = (line[i] + left) & 0xFF
            elif kind == 2:
                line[i] = (line[i] + up) & 0xFF
            elif kind == 3:
                line[i] = (line[i] + ((left + up) >> 1)) & 0xFF
            elif kind == 4:
                corner = previous[i - channels] if i >= channels else 0
                estimate = left + up - corner
                pa, pb, pc = abs(estimate - left), abs(estimate - up), abs(estimate - corner)
                predictor = left if pa <= pb and pa <= pc else (up if pb <= pc else corner)
                line[i] = (line[i] + predictor) & 0xFF
        rows.append(bytes(line))
        previous = line
    return width, height, channels, rows


# --- helpers -----------------------------------------------------------------

class ImageFolder:
    """A stand-in for the staged folder or the mounted image."""

    def __init__(self, test):
        self.path = tempfile.mkdtemp(prefix="dmg-layout-")
        test.addCleanup(shutil.rmtree, self.path)

    def add(self, name, directory=False):
        target = os.path.join(self.path, name)
        if directory:
            os.makedirs(target)
        else:
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with open(target, "w") as handle:
                handle.write(name)
        return target

    def build_like_the_workflow(self):
        """What release.yml stages: app, Applications, background, documents."""
        source = tempfile.mkdtemp(prefix="dmg-layout-out-")
        try:
            for document in ALL_DOCUMENTS:
                with open(os.path.join(source, document), "w") as handle:
                    handle.write(document)
            self.add(dmg_layout.APP_NAME, directory=True)
            self.add("Applications", directory=True)  # a symlink on the Mac
            self.add(".background/background.png")
            dmg_layout.stage_documents(source, self.path)
        finally:
            shutil.rmtree(source)
        return self

    def save_store(self, data):
        with open(os.path.join(self.path, ".DS_Store"), "wb") as handle:
            handle.write(data)


def run_main(arguments):
    """dmg_layout.main without its messages, which on GitHub Actions would
    otherwise turn into error annotations on a passing CI run."""
    with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
        return dmg_layout.main(arguments)


def finder_store(placed, icon_size=dmg_layout.ICON_SIZE, split=None):
    records = [(".", "icvp", "blob", icon_view(icon_size))]
    records += [(name, "Iloc", "blob", iloc(x, y)) for name, (x, y) in placed.items()]
    return write_ds_store(records, split)


# --- tests -------------------------------------------------------------------

class LayoutTable(unittest.TestCase):
    def test_no_icon_covers_the_instructions_or_another_icon(self):
        self.assertEqual(dmg_layout.layout_problems(dmg_layout.positions()), [])

    def test_the_v1_13_25_icon_positions_are_rejected(self):
        shipped = {"README.md": (130, 450), "SUPPORT.md": (430, 319),
                   "RELEASE_CHECKLIST.md": (262, 319), "PRIVACY.md": (430, 37)}
        problems = dmg_layout.layout_problems(shipped)
        self.assertIn("SUPPORT.md sits on the first-launch Terminal command", problems)
        self.assertIn("RELEASE_CHECKLIST.md sits on the first-launch Terminal command", problems)
        self.assertIn("PRIVACY.md sits on the title and drag instruction", problems)
        self.assertIn("PRIVACY.md is partly outside the window", problems)

    def test_the_bottom_row_clears_the_last_instruction_line_and_the_title_bar(self):
        for name, centre in dmg_layout.positions().items():
            box = dmg_layout.icon_box(centre)
            self.assertLessEqual(box[3], dmg_layout.WINDOW_HEIGHT - dmg_layout.TITLE_BAR, name)

    def test_every_document_has_exactly_one_home(self):
        top = set(dmg_layout.TOP_LEVEL_DOCUMENTS)
        folder = set(dmg_layout.FOLDER_DOCUMENTS)
        self.assertFalse(top & folder)
        self.assertEqual(top | folder, {"README.md", "LICENSE", "LICENSING.md", "SUPPORT.md",
                                        "PRIVACY.md", "RELEASE_CHECKLIST.md"})
        for document in top:
            self.assertIn(document, dmg_layout.positions())
        self.assertIn(dmg_layout.MORE_DOCUMENTS, dmg_layout.positions())


class Background(unittest.TestCase):
    """The keep-out areas must cover what the committed background draws."""

    @classmethod
    def setUpClass(cls):
        cls.width, cls.height, cls.channels, cls.rows = read_png(BACKGROUND)

    def test_background_is_the_window_size(self):
        self.assertEqual((self.width, self.height),
                         (dmg_layout.WINDOW_WIDTH, dmg_layout.WINDOW_HEIGHT))
        width, height, _, _ = read_png(BACKGROUND_2X)
        self.assertEqual((width, height),
                         (2 * dmg_layout.WINDOW_WIDTH, 2 * dmg_layout.WINDOW_HEIGHT))

    def test_everything_drawn_is_inside_an_instruction_area(self):
        areas = [area for _, area in dmg_layout.INSTRUCTIONS]
        inked = {what: False for what, _ in dmg_layout.INSTRUCTIONS}
        stray = []
        c = self.channels
        for y, row in enumerate(self.rows):
            backdrop = row[0:3]  # the gradient is flat across each row
            for x in range(self.width):
                pixel = row[x * c:x * c + 3]
                if sum(abs(pixel[i] - backdrop[i]) for i in range(3)) <= 30:
                    continue
                inside = [what for what, area in dmg_layout.INSTRUCTIONS
                          if area[0] <= x < area[2] and area[1] <= y < area[3]]
                if inside:
                    inked[inside[0]] = True
                else:
                    stray.append((x, y))
        self.assertEqual(stray[:10], [], "drawn outside every INSTRUCTIONS area")
        self.assertEqual([what for what, seen in inked.items() if not seen], [],
                         "an INSTRUCTIONS area with nothing drawn in it is stale")
        self.assertEqual(len(areas), len(inked))


class Staging(unittest.TestCase):
    def test_the_workflow_staging_has_a_place_for_everything(self):
        folder = ImageFolder(self).build_like_the_workflow()
        self.assertEqual(dmg_layout.contents_problems(folder.path), [])
        self.assertEqual(run_main(["check-staging", folder.path]), 0)
        for document in dmg_layout.TOP_LEVEL_DOCUMENTS:
            self.assertTrue(os.path.isfile(os.path.join(folder.path, document)))
        for document in dmg_layout.FOLDER_DOCUMENTS:
            self.assertTrue(os.path.isfile(
                os.path.join(folder.path, dmg_layout.MORE_DOCUMENTS, document)))

    def test_the_v1_13_25_staging_is_refused(self):
        folder = ImageFolder(self)
        folder.add(dmg_layout.APP_NAME, directory=True)
        folder.add("Applications", directory=True)
        for document in ALL_DOCUMENTS:
            folder.add(document)
        problems = dmg_layout.contents_problems(folder.path)
        for document in ("SUPPORT.md", "PRIVACY.md", "RELEASE_CHECKLIST.md", "LICENSING.md"):
            self.assertTrue(any(p.startswith(document + " has no place") for p in problems),
                            problems)
        self.assertEqual(run_main(["check-staging", folder.path]), 1)

    def test_a_new_unplaced_item_is_refused_and_hidden_items_are_ignored(self):
        folder = ImageFolder(self).build_like_the_workflow()
        folder.add(".fseventsd", directory=True)
        self.assertEqual(dmg_layout.contents_problems(folder.path), [])
        folder.add("NOTES.md")
        self.assertEqual(dmg_layout.contents_problems(folder.path),
                         ["NOTES.md has no place in the window layout (Tools/dmg_layout.py), "
                          "so Finder would put it wherever it likes"])

    def test_a_missing_folder_document_is_refused(self):
        folder = ImageFolder(self).build_like_the_workflow()
        os.remove(os.path.join(folder.path, dmg_layout.MORE_DOCUMENTS, "PRIVACY.md"))
        self.assertEqual(dmg_layout.contents_problems(folder.path),
                         ["PRIVACY.md is missing from More documents"])


class FinderScript(unittest.TestCase):
    def test_script_places_every_staged_item(self):
        folder = ImageFolder(self).build_like_the_workflow()
        script = dmg_layout.applescript()
        for name in dmg_layout.visible_items(folder.path):
            x, y = dmg_layout.positions()[name]
            self.assertIn('    set position of item "%s" of container window to {%d, %d}\n'
                          % (name, x, y), script)
        self.assertEqual(script.count("set position of item"),
                         len(dmg_layout.visible_items(folder.path)))

    def test_script_matches_the_background_and_icon_size(self):
        script = dmg_layout.applescript()
        left, top = dmg_layout.WINDOW_LEFT, dmg_layout.WINDOW_TOP
        self.assertIn("set the bounds of container window to {%d, %d, %d, %d}" % (
            left, top, left + dmg_layout.WINDOW_WIDTH, top + dmg_layout.WINDOW_HEIGHT), script)
        self.assertIn("set icon size of opts to %d\n" % dmg_layout.ICON_SIZE, script)
        self.assertIn("set arrangement of opts to not arranged\n", script)
        self.assertIn('set background picture of opts to file ".background:background.png"',
                      script)
        self.assertTrue(script.startswith('tell application "Finder"\n  tell disk "SobStage"\n'))
        self.assertTrue(script.endswith("  end tell\nend tell\n"))

    def test_names_are_quoted_for_applescript(self):
        script = dmg_layout.applescript('Sob "Stage"\\.app')
        self.assertIn('set position of item "Sob \\"Stage\\"\\\\.app" of container window', script)


class ShippedImage(unittest.TestCase):
    """The .DS_Store from the published v1.13.25 SobStage-macOS.dmg."""

    def test_reader_decodes_what_finder_saved(self):
        with open(SHIPPED_STORE, "rb") as handle:
            records = dmg_layout.read_ds_store(handle.read())
        self.assertEqual(dmg_layout.saved_positions(records), {
            "Applications": (495, 195), "LICENSE": (330, 450), "LICENSING.md": (530, 450),
            "PRIVACY.md": (430, 37), "README.md": (130, 450),
            "RELEASE_CHECKLIST.md": (262, 319), "SobStage.app": (165, 195),
            "SUPPORT.md": (430, 319)})
        self.assertEqual(dmg_layout.saved_icon_size(records), 108.0)

    def test_check_image_fails_the_shipped_window(self):
        folder = ImageFolder(self)
        folder.add(dmg_layout.APP_NAME, directory=True)
        folder.add("Applications", directory=True)
        for document in ALL_DOCUMENTS:
            folder.add(document)
        shutil.copy(SHIPPED_STORE, os.path.join(folder.path, ".DS_Store"))
        problems = dmg_layout.image_problems(folder.path)
        self.assertIn("SUPPORT.md sits on the first-launch Terminal command", problems)
        self.assertIn("RELEASE_CHECKLIST.md sits on the first-launch Terminal command", problems)
        self.assertIn("PRIVACY.md sits on the title and drag instruction", problems)
        self.assertEqual(run_main(["check-image", folder.path]), 1)


class FinishedImage(unittest.TestCase):
    def test_the_intended_window_passes(self):
        for split in (None, 2):
            folder = ImageFolder(self).build_like_the_workflow()
            folder.save_store(finder_store(dmg_layout.positions(), split=split))
            notes = []
            self.assertEqual(dmg_layout.image_problems(folder.path, notes=notes), [], split)
            self.assertEqual(notes, [])
            self.assertEqual(run_main(["check-image", folder.path]), 0)

    def test_reader_walks_an_internal_node(self):
        placed = dmg_layout.positions()
        records = dmg_layout.read_ds_store(finder_store(placed, split=3))
        self.assertEqual(dmg_layout.saved_positions(records), placed)

    def test_an_item_without_a_saved_position_fails(self):
        folder = ImageFolder(self).build_like_the_workflow()
        placed = dict(dmg_layout.positions())
        del placed[dmg_layout.MORE_DOCUMENTS]
        folder.save_store(finder_store(placed))
        self.assertEqual(dmg_layout.image_problems(folder.path),
                         ["More documents has no saved position, so Finder will put it "
                          "wherever it likes"])

    def test_an_icon_finder_moved_onto_the_command_fails(self):
        folder = ImageFolder(self).build_like_the_workflow()
        placed = dict(dmg_layout.positions())
        placed["LICENSE"] = (330, 344)
        folder.save_store(finder_store(placed))
        notes = []
        problems = dmg_layout.image_problems(folder.path, notes=notes)
        self.assertIn("LICENSE sits on the first-launch Terminal command", problems)
        self.assertEqual(notes, ["LICENSE is at (330, 344), not (330, 470)"])

    def test_a_harmless_nudge_is_only_a_note(self):
        folder = ImageFolder(self).build_like_the_workflow()
        placed = dict(dmg_layout.positions())
        placed["LICENSE"] = (334, 472)
        folder.save_store(finder_store(placed))
        notes = []
        self.assertEqual(dmg_layout.image_problems(folder.path, notes=notes), [])
        self.assertEqual(notes, ["LICENSE is at (334, 472), not (330, 470)"])

    def test_a_larger_saved_icon_size_is_used(self):
        folder = ImageFolder(self).build_like_the_workflow()
        folder.save_store(finder_store(dmg_layout.positions(), icon_size=160))
        self.assertTrue(any("sits on the" in problem
                            for problem in dmg_layout.image_problems(folder.path)))

    def test_a_plain_window_is_left_to_the_workflow_warning(self):
        folder = ImageFolder(self).build_like_the_workflow()
        self.assertEqual(dmg_layout.image_problems(folder.path), [])

    def test_an_unreadable_layout_fails(self):
        folder = ImageFolder(self).build_like_the_workflow()
        folder.save_store(b"\x00\x00\x00\x01Bud1" + b"\xff" * 40)
        problems = dmg_layout.image_problems(folder.path)
        self.assertEqual(len(problems), 1)
        self.assertTrue(problems[0].startswith("the saved window layout (.DS_Store) cannot be read"),
                        problems)


if __name__ == "__main__":
    unittest.main()
