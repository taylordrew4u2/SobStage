#!/usr/bin/env python3
"""The macOS disk-image window: where every icon sits, and the checks that keep
the icons off the instructions drawn on its background.

The background (Tools/make_dmg_background.py) is the only place a Mac user sees
the one step they must take before the first launch: the xattr command that
stops "SobStage is damaged and can't be opened". An icon on top of that command
hides it. v1.13.25 shipped exactly that. The release workflow staged six
documents but placed only three, and Finder put SUPPORT.md and
RELEASE_CHECKLIST.md on the command and PRIVACY.md on the title.

This table is now the one description of the window, and the release workflow
uses it for every step:

  stage-documents SRC DEST   copy the documents into the image folder
  check-staging DIR          every visible item has a place and every place
                             has an item (before the image is built)
  applescript                the Finder script that lays the window out
  check-image MOUNTPOINT     read the finished image's .DS_Store and fail if an
                             icon has no saved position or covers the
                             instructions

It runs on the macOS runner's stock python3, so it uses the standard library
only and stays compatible with Python 3.8.
"""

import argparse
import os
import plistlib
import shutil
import struct
import sys

VOLUME_NAME = "SobStage"
APP_NAME = "SobStage.app"

# The Finder window, in screen points: top-left corner and size. The size is
# also the background's size at 1x.
WINDOW_LEFT, WINDOW_TOP = 200, 120
WINDOW_WIDTH, WINDOW_HEIGHT = 660, 590

# Finder rounds icon sizes to a multiple of 4. v1.13.25 asked for 110 and
# stored 108, so ask for what is stored.
ICON_SIZE = 108

# Room under each icon for its name, which Finder wraps onto up to two lines.
LABEL_HEIGHT = 32

# Finder can count its title bar inside the window bounds, which hides this
# much of the bottom of the background. Nothing is placed there.
TITLE_BAR = 28

# README.md and LICENSE sit beside the app. The other four documents go in one
# folder: six loose documents do not fit under the instructions.
MORE_DOCUMENTS = "More documents"
TOP_LEVEL_DOCUMENTS = ("README.md", "LICENSE")
FOLDER_DOCUMENTS = ("LICENSING.md", "SUPPORT.md", "PRIVACY.md", "RELEASE_CHECKLIST.md")

# Icon centres in window points, measured from the background's top-left. This
# is the same coordinate Finder's "set position" takes and its .DS_Store stores.
POSITIONS = (
    (APP_NAME, (165, 195)),
    ("Applications", (495, 195)),
    ("README.md", (130, 470)),
    ("LICENSE", (330, 470)),
    (MORE_DOCUMENTS, (530, 470)),
)

# Everything the background draws, as (left, top, right, bottom) in window
# points. Tests/test_dmg_layout.py checks that every drawn pixel of
# Resources/DmgBackground.png falls inside one of these, so the text cannot move
# without these moving with it.
INSTRUCTIONS = (
    ("title and drag instruction", (0, 38, 660, 100)),
    ("arrow", (262, 188, 398, 222)),
    ("first-launch Terminal command", (0, 312, 660, 410)),
)


def positions(app_name=APP_NAME):
    """Icon centre for each visible item in the image, by name."""
    return {(app_name if name == APP_NAME else name): centre for name, centre in POSITIONS}


def icon_box(centre, icon_size=ICON_SIZE):
    """The area an icon and its name take up, as (left, top, right, bottom)."""
    x, y = centre
    half = icon_size / 2.0
    return (x - half, y - half, x + half, y + half + LABEL_HEIGHT)


def _overlaps(a, b):
    return a[0] < b[2] and b[0] < a[2] and a[1] < b[3] and b[1] < a[3]


def layout_problems(placed, icon_size=ICON_SIZE):
    """Every way these icon centres would hide the instructions or each other."""
    problems = []
    visible = (0, 0, WINDOW_WIDTH, WINDOW_HEIGHT - TITLE_BAR)
    boxes = sorted((name, icon_box(centre, icon_size)) for name, centre in placed.items())
    for name, box in boxes:
        for what, area in INSTRUCTIONS:
            if _overlaps(box, area):
                problems.append("%s sits on the %s" % (name, what))
        if (box[0] < visible[0] or box[1] < visible[1]
                or box[2] > visible[2] or box[3] > visible[3]):
            problems.append("%s is partly outside the window" % name)
    for index, (first, first_box) in enumerate(boxes):
        for second, second_box in boxes[index + 1:]:
            if _overlaps(first_box, second_box):
                problems.append("%s and %s overlap" % (first, second))
    return problems


def visible_items(directory):
    """What Finder shows at the top of the image: everything not hidden by a dot."""
    return sorted(name for name in os.listdir(directory) if not name.startswith("."))


def contents_problems(directory, app_name=APP_NAME):
    """Every visible item has a place, every place has an item, every document is there."""
    expected = positions(app_name)
    present = visible_items(directory)
    problems = []
    for name in present:
        if name not in expected:
            problems.append("%s has no place in the window layout (Tools/dmg_layout.py), "
                            "so Finder would put it wherever it likes" % name)
    for name in sorted(expected):
        if name not in present:
            problems.append("%s has a place in the window layout but is not in the image" % name)
    for document in FOLDER_DOCUMENTS:
        if not os.path.isfile(os.path.join(directory, MORE_DOCUMENTS, document)):
            problems.append("%s is missing from %s" % (document, MORE_DOCUMENTS))
    return problems


def stage_documents(source, destination):
    """Copy the documents from SOURCE into the image folder DESTINATION."""
    for document in TOP_LEVEL_DOCUMENTS:
        shutil.copy(os.path.join(source, document), os.path.join(destination, document))
    folder = os.path.join(destination, MORE_DOCUMENTS)
    if not os.path.isdir(folder):
        os.mkdir(folder)
    for document in FOLDER_DOCUMENTS:
        shutil.copy(os.path.join(source, document), os.path.join(folder, document))


def _applescript_string(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def applescript(app_name=APP_NAME):
    """The Finder script that opens the image's window and lays it out."""
    lines = [
        'tell application "Finder"',
        '  tell disk %s' % _applescript_string(VOLUME_NAME),
        '    open',
        '    set current view of container window to icon view',
        '    set toolbar visible of container window to false',
        '    set statusbar visible of container window to false',
        '    set the bounds of container window to {%d, %d, %d, %d}' % (
            WINDOW_LEFT, WINDOW_TOP, WINDOW_LEFT + WINDOW_WIDTH, WINDOW_TOP + WINDOW_HEIGHT),
        '    set opts to the icon view options of container window',
        '    set arrangement of opts to not arranged',
        '    set icon size of opts to %d' % ICON_SIZE,
        '    set background picture of opts to file ".background:background.png"',
    ]
    for name, (x, y) in positions(app_name).items():
        lines.append('    set position of item %s of container window to {%d, %d}'
                     % (_applescript_string(name), x, y))
    lines += [
        '    update without registering applications',
        '    close',
        '  end tell',
        'end tell',
    ]
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# .DS_Store ("Bud1") reader: just enough to read back what Finder saved.
#
# The file is a 4-byte prefix, then a buddy allocator whose offsets count from
# byte 4. Its table of contents names the "DSDB" block, which holds the root of
# a B-tree of records. Each record is (file name, 4-byte code, 4-byte type,
# value). The codes read here are "Iloc" (an icon's position: x and y as
# 32-bit integers, then 8 bytes Finder does not document) and "icvp" (the
# icon view options, a binary property list that includes iconSize).

_VALUE_SIZES = {"bool": 1, "long": 4, "shor": 4, "type": 4, "comp": 8, "dutc": 8}


def read_ds_store(data):
    """Return {(file name, code): (type, value)} for every record in DATA."""
    if data[:8] != b"\x00\x00\x00\x01Bud1":
        raise ValueError("not a .DS_Store file")
    root_offset, _root_size, root_check = struct.unpack_from(">III", data, 8)
    if root_offset != root_check:
        raise ValueError("damaged header")

    p = root_offset + 4
    count, _unused = struct.unpack_from(">II", data, p)
    addresses = struct.unpack_from(">%dI" % count, data, p + 8)
    p += 8 + 4 * ((count + 255) // 256 * 256)
    (directories,) = struct.unpack_from(">I", data, p)
    p += 4
    table = {}
    for _ in range(directories):
        length = data[p]
        name = data[p + 1:p + 1 + length].decode("ascii")
        (table[name],) = struct.unpack_from(">I", data, p + 1 + length)
        p += 1 + length + 4
    if "DSDB" not in table:
        raise ValueError("no DSDB block")

    def block(number):
        if number >= len(addresses):
            raise ValueError("block %d does not exist" % number)
        return (addresses[number] & ~0x1F) + 4

    (root_node,) = struct.unpack_from(">I", data, block(table["DSDB"]))
    records = {}

    def read_record(p):
        (length,) = struct.unpack_from(">I", data, p)
        p += 4
        name = data[p:p + 2 * length].decode("utf-16-be")
        p += 2 * length
        code = data[p:p + 4].decode("latin-1")
        kind = data[p + 4:p + 8].decode("latin-1")
        p += 8
        if kind in _VALUE_SIZES:
            size = _VALUE_SIZES[kind]
            value = data[p:p + size]
        elif kind == "blob":
            (size,) = struct.unpack_from(">I", data, p)
            p += 4
            value = data[p:p + size]
        elif kind == "ustr":
            (size,) = struct.unpack_from(">I", data, p)
            p += 4
            size *= 2
            value = data[p:p + size].decode("utf-16-be")
        else:
            raise ValueError("unknown record type %r" % kind)
        if p + size > len(data):
            raise ValueError("record runs past the end of the file")
        records[(name, code)] = (kind, value)
        return p + size

    def read_node(number, depth):
        if depth > 32:
            raise ValueError("the record tree loops")
        p = block(number)
        rightmost, count = struct.unpack_from(">II", data, p)
        p += 8
        for _ in range(count):
            if rightmost:
                (child,) = struct.unpack_from(">I", data, p)
                p += 4
                read_node(child, depth + 1)
            p = read_record(p)
        if rightmost:
            read_node(rightmost, depth + 1)

    read_node(root_node, 0)
    return records


def saved_positions(records):
    """Icon centre Finder saved for each item, by name."""
    placed = {}
    for (name, code), (kind, value) in records.items():
        if code == "Iloc" and kind == "blob" and len(value) >= 8:
            placed[name] = struct.unpack(">ii", value[:8])
    return placed


def saved_icon_size(records):
    """The icon size Finder saved for the window, or None."""
    kind, value = records.get((".", "icvp"), (None, None))
    if kind != "blob":
        return None
    try:
        size = plistlib.loads(value).get("iconSize")
    except Exception:  # plistlib raises several types for a damaged plist
        return None
    return float(size) if isinstance(size, (int, float)) else None


def image_problems(mountpoint, app_name=APP_NAME, notes=None):
    """Everything wrong with the window of the image mounted at MOUNTPOINT.

    Problems fail the release. NOTES, if given, collects differences that do
    not hide anything (an icon a few points from where it was asked to be).
    """
    problems = contents_problems(mountpoint, app_name)
    store = os.path.join(mountpoint, ".DS_Store")
    if not os.path.isfile(store):
        return problems  # A plain window: nothing is drawn behind the icons.
    try:
        with open(store, "rb") as handle:
            records = read_ds_store(handle.read())
    except (ValueError, struct.error, IndexError, UnicodeDecodeError) as error:
        return problems + ["the saved window layout (.DS_Store) cannot be read: %s" % error]

    saved = saved_positions(records)
    placed = {}
    for name in visible_items(mountpoint):
        if name in saved:
            placed[name] = saved[name]
        else:
            problems.append("%s has no saved position, so Finder will put it wherever it likes"
                            % name)
    problems += layout_problems(placed, saved_icon_size(records) or ICON_SIZE)

    expected = positions(app_name)
    for name, centre in sorted(placed.items()):
        if name in expected and tuple(centre) != tuple(expected[name]):
            if notes is not None:
                notes.append("%s is at %s, not %s" % (name, tuple(centre), tuple(expected[name])))
    return problems


def _report(kind, message):
    if os.environ.get("GITHUB_ACTIONS") == "true":
        print("::%s::%s" % (kind, message))
    else:
        print("%s: %s" % (kind, message), file=sys.stderr)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command")
    commands.required = True

    stage = commands.add_parser("stage-documents", help="copy the documents into the image folder")
    stage.add_argument("source")
    stage.add_argument("destination")

    for name, help_text in (("check-staging", "check the folder the image is built from"),
                            ("check-image", "check a mounted, finished image")):
        command = commands.add_parser(name, help=help_text)
        command.add_argument("directory")
        command.add_argument("--app", default=APP_NAME)

    script = commands.add_parser("applescript", help="print the Finder layout script")
    script.add_argument("--app", default=APP_NAME)

    arguments = parser.parse_args(argv)

    if arguments.command == "stage-documents":
        stage_documents(arguments.source, arguments.destination)
        return 0
    if arguments.command == "applescript":
        sys.stdout.write(applescript(arguments.app))
        return 0

    notes = []
    if arguments.command == "check-staging":
        problems = contents_problems(arguments.directory, arguments.app)
        problems += layout_problems(positions(arguments.app))
    else:
        problems = image_problems(arguments.directory, arguments.app, notes)
    for note in notes:
        _report("warning", "Disk image window: " + note)
    for problem in problems:
        _report("error", "Disk image window: " + problem)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
