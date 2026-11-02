"""Builds assets/atrium.ico and assets/atrium-256.png from the master image.

The mark (D42): two glass blades crossing into an A, its crossbar a frosted
capsule (the bar), on a white tile. Master: assets/atrium.svg, rendered at
1024 px to assets/atrium-1024.png (any browser; transparent background).
Needs Pillow. Usage: python tools/make_icon.py
"""

import os

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]


def main():
    master = Image.open(os.path.join(ROOT, "assets", "atrium-1024.png")).convert("RGBA")
    # The tile fills 824/1024 of the canvas; small sizes crop the shadow margin so the tile reads larger.
    images = []
    for s in SIZES:
        src = master
        if s <= 32:
            m = 84
            src = master.crop((m, m, 1024 - m, 1024 - m))
        images.append(src.resize((s, s), Image.LANCZOS))
    images[-1].save(os.path.join(ROOT, "assets", "atrium-256.png"))
    images[-1].save(os.path.join(ROOT, "assets", "atrium.ico"), sizes=[(s, s) for s in SIZES], append_images=images[:-1])
    print("wrote assets/atrium.ico and assets/atrium-256.png")


if __name__ == "__main__":
    main()
