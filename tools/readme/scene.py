"""Pieces of a scene: the bar and the widgets as glass over a backdrop.

Lengths are DIP; `s` is px per DIP (2 = a 200 % display).
"""

import math

import numpy as np
from PIL import Image, ImageFilter

import glass
from draw import Canvas, Theme, alpha, over, rgba
import faces

DARK_INK_ABOVE = 0.50  # bar.cpp kDarkInkAbove


def theme_for(luma):
    return Theme(luma > DARK_INK_ABOVE)


def widget(img, bd, x, y, w, h, paint, s=2.0, mat=glass.LIQUID, radius=22, force_dark=None):
    """A widget: a piece of glass (radius 22) and its face."""
    luma = glass.render(img, bd, (x * s, y * s, (x + w) * s, (y + h) * s), radius * s, mat, s)
    t = Theme(force_dark if force_dark is not None else luma > DARK_INK_ABOVE)
    c = Canvas(w, h, s, theme=t)
    paint(c, t)
    over(img, c.result(), x * s, y * s)
    return t


def wave_area(x):
    if x >= 1:
        return 0.5
    if x <= -1:
        return -0.5
    return x / 2 + math.sin(math.pi * x) / (2 * math.pi)


class Bar:
    """Bar::Relayout's metrics at the default size (height 60, icons 30) and BarPainter's wave."""

    def __init__(self, apps, time='10:24', date='2026/11/3', height=60, icon=30, mag=1.7, pointer=None, running=(), active=None,
                 status=None, s=2.0, mat=glass.LIQUID, hop=None):
        self.apps, self.time, self.date = apps, time, date
        self.H, self.icon, self.mag, self.s, self.mat = height, icon, mag, s, mat
        self.pointer, self.running, self.active, self.hop = pointer, set(running), active, hop
        self.status = status or {}
        self.plate = max(icon + 4, min(height - 12, round(icon * 1.55 + 1e-9)))
        self.gap = max(4, round(icon * 0.18))
        self.sep = 8
        self.pad = (height - self.plate) // 2 + 4
        c = Canvas(10, 10, s)
        self.clockW = math.ceil(max(c.text_width(time.replace('1', '0'), height * 0.25, 'semibold'),
                                    c.text_width(date, height * 0.19, 'regular'))) + 22
        p, g = self.plate, self.gap
        n = len(apps)
        self.W = 2 * self.pad + p + 2 * g + (n * p + (n - 1) * g if n else 0) + 2 * self.sep + p + g + p + g + self.clockW + g + p
        self.pitch = p + g
        self.range = self.pitch * 2.3

    def map_x(self, u):
        if self.pointer is None or self.mag <= 1.001:
            return u
        k = self.mag - 1
        return u + k * self.range * wave_area((u - self.pointer) / self.range)

    def slots(self):
        """(key, left) of every plate, base coordinates."""
        p, g = self.plate, self.gap
        out = [('#start', self.pad)]
        apps_left = self.pad + p + 2 * g
        for k, a in enumerate(self.apps):
            out.append((a, apps_left + k * self.pitch))
        bell = self.W - self.pad - p
        clock_right = bell - g
        clock_left = clock_right - self.clockW
        status_left = clock_left - g - p
        tray_left = status_left - g - p
        self.clock = (clock_left, clock_right)
        self.sep_x = tray_left - self.sep
        out += [('#tray', tray_left), ('#status', status_left), ('#notify', bell)]
        return out

    def app_x(self, index):
        """The base centre of app `index`."""
        return self.pad + self.plate + 2 * self.gap + index * self.pitch + self.plate / 2

    def draw(self, img, bd, cx, bottom):
        """Draws the bar centred at cx with its lower edge at `bottom` (DIP)."""
        s = self.s
        x0 = cx - self.W / 2
        top = bottom - self.H
        slots = self.slots()
        L, R = self.map_x(0), self.map_x(self.W)
        rect = ((x0 + L) * s, top * s, (x0 + R) * s, bottom * s)
        luma = glass.render(img, bd, rect, min(30, self.H / 2) * s, self.mat, s)
        t = Theme(luma > DARK_INK_ABOVE)
        ink = t.text
        rise = self.icon * (self.mag - 1) + 12
        cw = R - L + 40
        c = Canvas(cw, self.H + rise + 4, s, theme=t)
        ox, oy = L - 20, -rise  # canvas origin in bar coordinates
        icon_top = (self.H - self.icon) / 2
        icon_bottom = icon_top + self.icon
        for key, left in slots:
            bx = left + self.plate / 2
            pw = self.plate
            grow = (self.map_x(bx + pw / 2) - self.map_x(bx - pw / 2)) / pw
            dcx = self.map_x(bx)
            size = self.icon * grow
            up = 0.0
            if self.hop is not None and key == self.hop[0]:
                up = self.hop[1] * 0.32 * self.icon
            l, b = dcx - size / 2 - ox, icon_bottom - oy - up
            r_ = (l, b - size, l + size, b)
            if key == '#start':
                faces.windows_logo(c, r_)
            elif key == '#tray':
                c.glyph('', r_, size * 0.62, ink)
            elif key == '#notify':
                c.glyph('', r_, size * 0.62, ink)
            elif key == '#status':
                faces.status_ring(c, (r_[0] + r_[2]) / 2, (r_[1] + r_[3]) / 2, size, ink, **self.status)
            else:
                icon = faces.app_icon(key)
                self._shadowed(c, icon, r_, s)
                if key in self.running:
                    active = key == self.active
                    dot = alpha(ink, 1 if active else 0.45)
                    c.circle(dcx - ox, (icon_bottom + self.H) / 2 - oy, 2.3 if active else 1.9, dot)
        # The separator, the clock.
        sx = self.map_x(self.sep_x) - ox
        hgt = self.H * 0.46
        c.fill((sx - 0.5, self.H / 2 - hgt / 2 - oy, sx + 0.5, self.H / 2 + hgt / 2 - oy), 0, alpha(ink, 0.30 / 0.96 if not t.dark else 0.18 / 0.92))
        ccx = self.map_x((self.clock[0] + self.clock[1]) / 2) - ox
        tp, dp = self.H * 0.25, self.H * 0.19
        lt, ld = tp * 1.22, dp * 1.25
        y0 = self.H / 2 - (lt + ld) / 2 - oy
        half = self.clockW / 2
        c.text(self.time, (ccx - half, y0, ccx + half, y0 + lt), tp, ink, 'semibold', 1, bump=False)
        c.text(self.date, (ccx - half, y0 + lt, ccx + half, y0 + lt + ld), dp, ink, 'regular', 1, bump=False)
        over(img, c.result(), (x0 + ox) * s, (top + oy) * s)
        return t

    @staticmethod
    def _shadowed(c, icon, r_, s):
        l, t, r, b = r_
        size = r - l
        k = c.k
        px = int(round(size * k))
        im = icon.convert('RGBa').resize((px, px), Image.LANCZOS).convert('RGBA')
        # The icon's drop shadow: blur 1.6 DIP, 38 % black, 1.2 DIP down.
        pad = int(6 * k)
        sh = Image.new('L', (px + 2 * pad, px + 2 * pad), 0)
        sh.paste(im.getchannel('A'), (pad, pad))
        sh = sh.filter(ImageFilter.GaussianBlur(1.6 * k))
        sh = sh.point(lambda v: int(v * 0.38))
        layer = Image.new('RGBA', sh.size, (0, 0, 0, 0))
        layer.putalpha(sh)
        c.img.alpha_composite(layer, (int(round(l * k)) - pad, int(round((t + 1.2) * k)) - pad))
        c.img.alpha_composite(im, (int(round(l * k)), int(round(t * k))))
