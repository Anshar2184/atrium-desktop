"""Vector drawing for the artwork, in DIP, supersampled.

A Canvas covers one element (a widget, the bar's content) at `scale` px per
DIP and draws at `ss` times that, then is reduced with a Lanczos filter.
Primitives blend one at a time, so translucent ink overlaps as in Direct2D.
Fonts and inks follow the core: MiSans in its three files, the glass inks of
Theme::For, the halo just below glass text, heavier small type.
"""

import math
import os

import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), '..', '..'))
FONT_FILES = {
    'regular': os.path.join(ROOT, 'assets', 'fonts', 'MiSans-Medium.otf'),     # OS/2 380
    'medium': os.path.join(ROOT, 'assets', 'fonts', 'MiSans-Demibold.otf'),    # 450
    'semibold': os.path.join(ROOT, 'assets', 'fonts', 'MiSans-Semibold.otf'),  # 520
}
ICON_FONT = '/mnt/c/Windows/Fonts/SegoeIcons.ttf'

_fonts = {}


def font(px, weight='regular', icons=False):
    key = (round(px * 4), weight, icons)
    if key not in _fonts:
        path = ICON_FONT if icons else FONT_FILES[weight]
        _fonts[key] = ImageFont.truetype(path, max(1, int(round(px))), layout_engine=ImageFont.Layout.RAQM)
    return _fonts[key]


def rgba(hex_rgb, a=1.0):
    return ((hex_rgb >> 16) & 255, (hex_rgb >> 8) & 255, hex_rgb & 255, int(round(a * 255)))


def alpha(c, a):
    return (c[0], c[1], c[2], int(round(c[3] * a)))


class Theme:
    """Theme::For: the inks over glass."""

    def __init__(self, dark_ink):
        self.dark = dark_ink
        if dark_ink:
            self.text, self.sub, self.faint = rgba(0x111317, .92), rgba(0x111317, .66), rgba(0x111317, .46)
            self.hover, self.divider, self.card = rgba(0, .10), rgba(0, .12), rgba(0, .05)
            self.halo = rgba(0xFFFFFF, .35)
        else:
            self.text, self.sub, self.faint = rgba(0xFFFFFF, .96), rgba(0xFFFFFF, .76), rgba(0xFFFFFF, .56)
            self.hover, self.divider, self.card = rgba(0xFFFFFF, .20), rgba(0xFFFFFF, .24), rgba(0xFFFFFF, .08)
            self.halo = rgba(0, .32)
        # The widgets' palette (widget_faces GlassPal): macOS's accents.
        self.face = alpha(self.text, 0.10)
        self.track = alpha(self.text, 0.18)
        self.red, self.orange, self.green, self.blue = rgba(0xFF453A, .98), rgba(0xFF9F0A, .98), rgba(0x30D158, .98), rgba(0x0A84FF, .98)


def glass_weight(size, weight):
    """GlassWeight: 12.5 DIP and below one step heavier."""
    if size <= 12.5:
        return {'regular': 'medium', 'medium': 'semibold'}.get(weight, weight)
    return weight


class Canvas:
    def __init__(self, w, h, scale=2.0, ss=3, theme=None, halo=True):
        self.w, self.h, self.scale, self.ss = w, h, scale, ss
        self.k = scale * ss
        self.img = Image.new('RGBA', (int(math.ceil(w * self.k)), int(math.ceil(h * self.k))), (0, 0, 0, 0))
        self.theme = theme
        self.halo = halo

    # -- blending -------------------------------------------------------
    def _layer(self, box, colour, paint):
        """Paints a coverage mask over `box` (px) with fill 255 and blends `colour` through it."""
        x0, y0, x1, y1 = [int(math.floor(box[0])) - 2, int(math.floor(box[1])) - 2, int(math.ceil(box[2])) + 2, int(math.ceil(box[3])) + 2]
        x0, y0 = max(x0, 0), max(y0, 0)
        x1, y1 = min(x1, self.img.width), min(y1, self.img.height)
        if x1 <= x0 or y1 <= y0 or colour[3] <= 0:
            return
        mask = Image.new('L', (x1 - x0, y1 - y0), 0)
        paint(ImageDraw.Draw(mask), x0, y0)
        if colour[3] < 255:
            mask = mask.point(lambda v: v * colour[3] // 255)
        layer = Image.new('RGBA', mask.size, tuple(colour[:3]) + (0,))
        layer.putalpha(mask)
        self.img.alpha_composite(layer, (x0, y0))

    def P(self, v):
        return v * self.k

    # -- shapes ---------------------------------------------------------
    def fill(self, rect, radius, colour):
        l, t, r, b = [self.P(v) for v in rect]
        rad = self.P(radius)
        self._layer((l, t, r, b), colour, lambda d, ox, oy: d.rounded_rectangle((l - ox, t - oy, r - ox - 1, b - oy - 1), rad, fill=255))

    def circle(self, cx, cy, r, colour):
        cx, cy, r = self.P(cx), self.P(cy), self.P(r)
        self._layer((cx - r, cy - r, cx + r, cy + r), colour, lambda d, ox, oy: d.ellipse((cx - r - ox, cy - r - oy, cx + r - ox, cy + r - oy), fill=255))

    def ring(self, cx, cy, r, colour, width):
        cx, cy, r, w = self.P(cx), self.P(cy), self.P(r), self.P(width)
        ro = r + w / 2
        self._layer((cx - ro, cy - ro, cx + ro, cy + ro), colour,
                    lambda d, ox, oy: d.ellipse((cx - ro - ox, cy - ro - oy, cx + ro - ox, cy + ro - oy), outline=255, width=int(round(w))))

    def capsule(self, x0, y0, x1, y1, colour, width):
        """A line with round caps."""
        a, b, c, e, w = self.P(x0), self.P(y0), self.P(x1), self.P(y1), self.P(width)

        def paint(d, ox, oy):
            d.line((a - ox, b - oy, c - ox, e - oy), fill=255, width=max(1, int(round(w))))
            for x, y in ((a, b), (c, e)):
                d.ellipse((x - w / 2 - ox, y - w / 2 - oy, x + w / 2 - ox, y + w / 2 - oy), fill=255)
        self._layer((min(a, c) - w, min(b, e) - w, max(a, c) + w, max(b, e) + w), colour, paint)

    def arc(self, cx, cy, r, a0, a1, colour, width, caps=True):
        """An arc clockwise from a0 to a1 (degrees, 0 = right, 90 = down), round caps."""
        if a1 - a0 <= 0.01:
            return
        cx, cy, r, w = self.P(cx), self.P(cy), self.P(r), self.P(width)
        ro = r + w / 2

        def paint(d, ox, oy):
            if a1 - a0 >= 359.99:
                d.ellipse((cx - ro - ox, cy - ro - oy, cx + ro - ox, cy + ro - oy), outline=255, width=int(round(w)))
                return
            d.arc((cx - ro - ox, cy - ro - oy, cx + ro - ox, cy + ro - oy), a0, a1, fill=255, width=int(round(w)))
            if caps:
                for a in (a0, a1):
                    x, y = cx + r * math.cos(math.radians(a)), cy + r * math.sin(math.radians(a))
                    d.ellipse((x - w / 2 - ox, y - w / 2 - oy, x + w / 2 - ox, y + w / 2 - oy), fill=255)
        self._layer((cx - ro - w, cy - ro - w, cx + ro + w, cy + ro + w), colour, paint)

    def polygon(self, pts, colour):
        pts = [(self.P(x), self.P(y)) for x, y in pts]
        xs, ys = [p[0] for p in pts], [p[1] for p in pts]
        self._layer((min(xs), min(ys), max(xs), max(ys)), colour, lambda d, ox, oy: d.polygon([(x - ox, y - oy) for x, y in pts], fill=255))

    def image(self, im, x, y, w, h):
        """Blends an RGBA image into the rect (DIP)."""
        im = im.convert('RGBa').resize((int(round(self.P(w))), int(round(self.P(h)))), Image.LANCZOS).convert('RGBA')
        self.img.alpha_composite(im, (int(round(self.P(x))), int(round(self.P(y)))))

    # -- type -----------------------------------------------------------
    def text(self, s, rect, size, colour, weight='regular', align=0, glass=True, valign='middle', tracking=0.0, features=('tnum',), bump=None):
        """Canvas::Text: one line, centred vertically in rect (DIP); align 0 left, 1 centre, 2 right."""
        if not s:
            return
        if glass if bump is None else bump:
            weight = glass_weight(size, weight)
        f = font(self.P(size), weight)
        l, t, r, b = [self.P(v) for v in rect]
        x = (l, (l + r) / 2, r)[align]
        anchor = ('l', 'm', 'r')[align] + ('m' if valign == 'middle' else 'a')
        y = (t + b) / 2 if valign == 'middle' else t
        o = max(1.0, self.scale * 0.6) * self.ss

        def draw_one(d, ox, oy, dy):
            if tracking:
                # Letter by letter, for tracked capitals.
                widths = [f.getlength(ch) for ch in s]
                total = sum(widths) + tracking * self.P(size) * (len(s) - 1)
                cx = x - (0, total / 2, total)[align]
                for ch, cw in zip(s, widths):
                    d.text((cx - ox, y - oy + dy), ch, font=f, fill=255, anchor='l' + anchor[1])
                    cx += cw + tracking * self.P(size)
            else:
                d.text((x - ox, y - oy + dy), s, font=f, fill=255, anchor=anchor, features=list(features))

        width = f.getlength(s, features=list(features)) + abs(tracking) * self.P(size) * len(s)
        x0 = x - (0, width / 2, width)[align]
        box = (x0 - 4, min(t, y) - self.P(size), x0 + width + 4, max(b, y) + 2 * self.P(size))
        if glass and self.halo and self.theme is not None:
            self._layer(box, alpha(self.theme.halo, colour[3] / 255), lambda d, ox, oy: draw_one(d, ox, oy, o))
        self._layer(box, colour, lambda d, ox, oy: draw_one(d, ox, oy, 0))

    def text_width(self, s, size, weight='regular'):
        return font(self.P(size), weight).getlength(s, features=['tnum']) / self.k

    def glyph(self, ch, rect, size, colour, halo=True):
        f = font(self.P(size), icons=True)
        l, t, r, b = [self.P(v) for v in rect]
        x, y = (l + r) / 2, (t + b) / 2
        o = max(1.0, self.scale * 0.6) * self.ss

        box = (l - self.P(size), t - self.P(size), r + self.P(size), b + self.P(size))
        if halo and self.halo and self.theme is not None:
            self._layer(box, alpha(self.theme.halo, colour[3] / 255), lambda d, ox, oy: d.text((x - ox, y - oy + o), ch, font=f, fill=255, anchor='mm'))
        self._layer(box, colour, lambda d, ox, oy: d.text((x - ox, y - oy), ch, font=f, fill=255, anchor='mm'))

    # -- output ---------------------------------------------------------
    def result(self):
        """The canvas at `scale` px per DIP (premultiplied while reduced)."""
        size = (int(math.ceil(self.w * self.scale)), int(math.ceil(self.h * self.scale)))
        return self.img.convert('RGBa').resize(size, Image.LANCZOS).convert('RGBA')


def over(dst, layer, x, y):
    """Blends an RGBA PIL layer onto a float image (H, W, 3) at px (x, y)."""
    a = np.asarray(layer, np.float32) / 255
    x, y = int(round(x)), int(round(y))
    h, w = a.shape[:2]
    H, W = dst.shape[:2]
    sx0, sy0 = max(0, -x), max(0, -y)
    dx0, dy0 = max(0, x), max(0, y)
    w2, h2 = min(w - sx0, W - dx0), min(h - sy0, H - dy0)
    if w2 <= 0 or h2 <= 0:
        return
    src = a[sy0:sy0 + h2, sx0:sx0 + w2]
    region = dst[dy0:dy0 + h2, dx0:dx0 + w2]
    region[:] = src[..., :3] * src[..., 3:] + region * (1 - src[..., 3:])


def to_image(img):
    return Image.fromarray((np.clip(img, 0, 1) * 255 + 0.5).astype(np.uint8))
