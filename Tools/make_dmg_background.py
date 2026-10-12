#!/usr/bin/env python3
"""Generate the macOS disk-image background.

The window a Mac user opens is the whole install experience, so it says what to
do rather than leaving them to guess: the app on the left, the Applications
folder on the right, an arrow between them, and the one first-launch step
underneath. Same §9.2 palette as the app and the icon.

The window size and every icon position live in Tools/dmg_layout.py, which the
release workflow also uses to place the icons and to check that none of them
covers this text. Move any text here and move its INSTRUCTIONS area there
(Tests/test_dmg_layout.py fails until both agree).

Run:  python3 Tools/make_dmg_background.py
Out:  Resources/DmgBackground.png  (WINDOW_WIDTH x WINDOW_HEIGHT @1x)
      Resources/DmgBackground@2x.png
"""

import os
from PIL import Image, ImageDraw, ImageFont

from dmg_layout import WINDOW_HEIGHT, WINDOW_WIDTH

# §9.2 palette, from the `palette` namespace in Source/UI/AppLookAndFeel.h.
BONE = (227, 234, 242)          # palette::bone
SECONDARY = (132, 150, 168)     # palette::secondary
TERTIARY = (86, 99, 114)        # palette::tertiary
ACCENT = (34, 211, 238)         # palette::accent
BACKDROP_TOP = (28, 39, 51)     # palette::surfaceHigh
BACKDROP_BOTTOM = (10, 14, 19)  # palette::background

W, H = WINDOW_WIDTH, WINDOW_HEIGHT
SS = 2


def load_font(size, bold=False, mono=False):
    # Liberation before DejaVu: the committed background was drawn with
    # Liberation Sans and Mono, and a machine with both fonts must redraw it
    # the same way.
    if mono:
        candidates = [
            "/System/Library/Fonts/SFNSMono.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        ]
    else:
        candidates = [
            "/System/Library/Fonts/Supplemental/Arial Bold.ttf" if bold
            else "/System/Library/Fonts/Supplemental/Arial.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf" if bold
            else "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf" if bold
            else "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        ]
    for path in candidates:
        if os.path.exists(path):
            return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def centred(d, y, text, font, fill):
    left, top, right, bottom = d.textbbox((0, 0), text, font=font)
    d.text(((W * SS - (right - left)) / 2 - left, y), text, font=font, fill=fill)


def main():
    img = Image.new("RGB", (W * SS, H * SS))
    px = img.load()
    for y in range(H * SS):
        t = y / (H * SS - 1)
        row = tuple(round(a + (b - a) * t) for a, b in zip(BACKDROP_TOP, BACKDROP_BOTTOM))
        for x in range(W * SS):
            px[x, y] = row

    d = ImageDraw.Draw(img)

    centred(d, 40 * SS, "SobStage", load_font(25 * SS, bold=True), BONE)
    centred(d, 78 * SS, "Drag the crying face onto Applications", load_font(14 * SS), SECONDARY)

    # Arrow between the app and Applications icons (x 165 and 495 in
    # Tools/dmg_layout.py).
    y = 205 * SS
    x0, x1 = 268 * SS, 392 * SS
    # The accent, not the dimmest tone available. This arrow is the entire
    # instruction the window exists to give, and it was the faintest mark on it.
    d.line([(x0, y), (x1 - 16 * SS, y)], fill=ACCENT, width=3 * SS)
    d.polygon([(x1, y), (x1 - 19 * SS, y - 11 * SS), (x1 - 19 * SS, y + 11 * SS)],
              fill=ACCENT)

    # The one step that is not obvious, said here rather than only in a README
    # nobody opens.
    #
    # This used to say "Control-click the app, then choose Open." That is the
    # workaround for an app that is unsigned; this one is ad-hoc signed, and
    # Control-click -> Open does not clear a quarantine flag on a build with no
    # Developer ID -- which is exactly the case someone hitting "is damaged" is
    # in. So the panel that a Mac user reads before anything else was giving
    # them the one instruction that could not work. The command below is what
    # actually works, and it is the same one the README gives.
    centred(d, 318 * SS, "First launch: after dragging, run this once in Terminal",
            load_font(13 * SS), BONE)
    centred(d, 344 * SS,
            'xattr -dr com.apple.quarantine "/Applications/SobStage.app"',
            load_font(11 * SS, mono=True), ACCENT)
    centred(d, 372 * SS, "Needed because this build has no paid Apple certificate.",
            load_font(11 * SS), TERTIARY)
    centred(d, 392 * SS, 'Without it macOS says "is damaged". Nothing is wrong with the download.',
            load_font(11 * SS), TERTIARY)

    out_dir = os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "Resources")
    os.makedirs(out_dir, exist_ok=True)

    img.resize((W, H), Image.LANCZOS).save(os.path.join(out_dir, "DmgBackground.png"))
    img.save(os.path.join(out_dir, "DmgBackground@2x.png"))
    print(f"wrote {out_dir}/DmgBackground.png and @2x")


if __name__ == "__main__":
    main()
