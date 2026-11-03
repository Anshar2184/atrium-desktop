"""What the core draws on the glass, redrawn for the artwork.

The widget faces follow native/core/ui/widget_faces.cpp, the status ring and
the Wi-Fi fan follow render/bar_painter.cpp and render/wifi_glyph.cpp, the
bar's metrics follow Bar::Relayout. The app icons are original drawings (the
artwork shows no third-party icons).
"""

import math

import numpy as np
from PIL import Image, ImageChops, ImageDraw

from draw import Canvas, Theme, alpha, font, rgba

PAD = 14  # widget_faces kPad
WEEK_LONG = ['星期一', '星期二', '星期三', '星期四', '星期五', '星期六', '星期日']
WEEK_SHORT = ['一', '二', '三', '四', '五', '六', '日']
MONTHS = ['一月', '二月', '三月', '四月', '五月', '六月', '七月', '八月', '九月', '十月', '十一月', '十二月']


def weekday(y, m, d):  # lunar.cpp Weekday: 0 = Monday
    t = [0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4]
    if m < 3:
        y -= 1
    return ((y + y // 4 - y // 100 + y // 400 + t[m - 1] + d) % 7 + 6) % 7


def days_in(y, m):
    leap = (y % 4 == 0 and y % 100 != 0) or y % 400 == 0
    return 29 if m == 2 and leap else [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31][m - 1]


# ---------------------------------------------------------------- widgets

def clock_face(c, cx, cy, R, hms, face, ink, numbers, label, label_ink, orange):
    h_, m_, s_ = hms
    c.circle(cx, cy, R, face)
    for h in range(1, 13):
        a = math.radians(h * 30 - 90)
        if numbers:
            r, fs = R * 0.78, R * 0.21
            x, y = cx + r * math.cos(a), cy + r * math.sin(a)
            c.text(str(h), (x - fs, y - fs * 0.72, x + fs, y + fs * 0.72), fs, ink, 'semibold', 1)
        else:
            r0, r1 = R * 0.80, R * 0.92
            c.capsule(cx + r0 * math.cos(a), cy + r0 * math.sin(a), cx + r1 * math.cos(a), cy + r1 * math.sin(a),
                      alpha(ink, 0.45 if h % 3 else 0.9), max(1, R * 0.045))
    if label:
        c.text(label, (cx - R, cy + R * 0.26, cx + R, cy + R * 0.26 + R * 0.2), R * 0.15, label_ink, 'semibold', 1)
    minute = m_ + s_ / 60
    hour = h_ % 12 + minute / 60

    def hand(angle, length, width):
        a = math.radians(angle - 90)
        neck = R * 0.16
        dx, dy = math.cos(a), math.sin(a)
        c.capsule(cx, cy, cx + dx * neck, cy + dy * neck, ink, max(1.2, width * 0.36))
        c.capsule(cx + dx * neck, cy + dy * neck, cx + dx * length, cy + dy * length, ink, width)
    hand(hour * 30, R * 0.52, max(2.5, R * 0.075))
    hand(minute * 6, R * 0.80, max(2.5, R * 0.075))
    c.circle(cx, cy, max(2.2, R * 0.055), ink)
    a = math.radians(s_ * 6 - 90)
    c.capsule(cx - math.cos(a) * R * 0.18, cy - math.sin(a) * R * 0.18, cx + math.cos(a) * R * 0.86, cy + math.sin(a) * R * 0.86,
              orange, max(1, R * 0.02))
    c.circle(cx, cy, max(1.8, R * 0.04), orange)
    c.circle(cx, cy, max(0.8, R * 0.016), face if face[3] > 200 else (0, 0, 0, 0))


def paint_clock_small(c, t, hms=(10, 9, 32), city='北京'):
    W, H = c.w, c.h
    clock_face(c, W / 2, H / 2, H / 2 - 9, hms, t.face, t.text, True, city, t.sub, t.orange)


def paint_world_clock(c, t, cities):
    """cities: (name, when, (h, m, s)); dark faces at night."""
    W, H = c.w, c.h
    colW, R = (W - 2 * PAD) / 4, 31
    for i, (name, when, hms) in enumerate(cities):
        night = hms[0] < 6 or hms[0] >= 18
        cx, cy = PAD + colW * (i + 0.5), (H - 2 * R - 42) / 2 + R - 2
        face = rgba(0x000000, 0.42) if night else t.face
        ink = rgba(0xFFFFFF, 0.96) if night else t.text
        clock_face(c, cx, cy, R, hms, face, ink, False, '', ink, t.orange)
        c.text(name, (cx - colW / 2, cy + R + 8, cx + colW / 2, cy + R + 26), 12.5, t.text, 'semibold', 1)
        c.text(when, (cx - colW / 2, cy + R + 26, cx + colW / 2, cy + R + 42), 10.5, t.sub, 'regular', 1)


def paint_calendar_medium(c, t, y, m, d, lunar, first=0):
    W, H = c.w, c.h
    wd = weekday(y, m, d)
    leftW = 126
    c.text(WEEK_LONG[wd], (PAD + 2, PAD, PAD + leftW, PAD + 20), 14, t.blue, 'semibold')
    c.text(str(d), (PAD, PAD + 16, PAD + leftW, PAD + 80), 54, t.text, 'regular')
    c.text('农历' + lunar, (PAD + 2, H - PAD - 40, PAD + leftW, H - PAD - 20), 12.5, t.text, 'semibold')
    c.text(f'{y}年{MONTHS[m - 1]}', (PAD + 2, H - PAD - 20, PAD + leftW, H - PAD), 12, t.sub, 'regular')
    gx = PAD + leftW + 6
    gw = W - PAD - gx
    cw = gw / 7
    c.text(MONTHS[m - 1], (gx + 4, PAD - 1, gx + gw, PAD + 17), 12.5, t.blue, 'semibold')
    yy = PAD + 20
    for i in range(7):
        c.text(WEEK_SHORT[(first + i) % 7], (gx + i * cw, yy, gx + (i + 1) * cw, yy + 14), 9.5, t.sub, 'semibold', 1)
    yy += 16
    lead = (weekday(y, m, 1) - first + 7) % 7
    days = days_in(y, m)
    rows = (lead + days + 6) // 7
    rh = min(17, (H - PAD - yy) / rows)
    for day in range(1, days + 1):
        cell = lead + day - 1
        x0, y0 = gx + (cell % 7) * cw, yy + (cell // 7) * rh
        cx, cy = x0 + cw / 2, y0 + rh / 2
        today = day == d
        if today:
            c.circle(cx, cy, rh / 2 + 0.5, t.blue)
        weekend = (first + cell % 7) % 7 >= 5
        ink = rgba(0xFFFFFF, 0.98) if today else t.sub if weekend else t.text
        c.text(str(day), (x0, cy - rh / 2, x0 + cw, cy + rh / 2), 10.5, ink, 'semibold', 1)


def paint_battery(c, t, pct, charging):
    W, H = c.w, c.h
    cx, cy, R = W / 2, H / 2 - 12, 44
    c.arc(cx, cy, R, -90, 270, t.track, 9, caps=False)
    level = t.red if pct <= 20 and not charging else t.green
    c.arc(cx, cy, R, -90, -90 + 360 * pct / 100, level, 9)
    c.glyph('', (cx - 22, cy - 22, cx + 22, cy + 22), 24, t.text)
    n = f'{pct}%'
    tw = c.text_width(n, 19, 'semibold')
    bolt = 16 if charging else 0
    x0 = (W - tw - bolt) / 2
    if charging:
        c.glyph('', (x0 - 2, cy + R + 10, x0 + bolt, cy + R + 36), 13, level)
    c.text(n, (x0 + bolt, cy + R + 10, x0 + bolt + tw + 4, cy + R + 36), 19, t.text, 'semibold')


def paint_system_medium(c, t, gauges):
    """gauges: (name, value 0..1, detail)."""
    W, H = c.w, c.h
    colW, R = (W - 2 * PAD) / 3, 34
    cy = PAD + 6 + R
    for i, (name, v, detail) in enumerate(gauges):
        cx = PAD + colW * (i + 0.5)
        colour = t.red if v >= 0.9 else t.orange if v >= 0.7 else t.green
        c.arc(cx, cy, R, -90, 270, t.track, 8, caps=False)
        c.arc(cx, cy, R, -90, -90 + 360 * v, colour, 8)
        c.text(f'{round(v * 100)}%', (cx - R, cy - 12, cx + R, cy + 12), 17, t.text, 'semibold', 1)
        c.text(name, (cx - colW / 2, cy + R + 8, cx + colW / 2, cy + R + 26), 13, t.text, 'semibold', 1)
        c.text(detail, (cx - colW / 2, cy + R + 26, cx + colW / 2, cy + R + 42), 10.5, t.sub, 'regular', 1)


def paint_digital(c, t, hm, day, date, city='本地'):
    W, H = c.w, c.h
    leftW = W - 2 * PAD
    c.text(city, (PAD + 2, PAD, PAD + leftW, PAD + 18), 12.5, t.sub, 'semibold')
    c.text(hm, (PAD, PAD + 22, PAD + leftW, PAD + 84), 50, t.text, 'semibold')
    c.text(day, (PAD + 2, H - PAD - 40, PAD + leftW, H - PAD - 20), 13, t.blue, 'semibold')
    c.text(date, (PAD + 2, H - PAD - 20, PAD + leftW, H - PAD), 13, t.text, 'regular')


def paint_reminders(c, t, items):
    """PaintReminders: the title and the open count in orange, then the list (rows of 27)."""
    W, H = c.w, c.h
    open_count = sum(1 for _, done in items if not done)
    c.text('提醒事项', (PAD + 2, PAD - 2, W - PAD - 40, PAD + 18), 13.5, t.orange, 'semibold')
    c.text(str(open_count), (W - PAD - 48, PAD - 6, W - PAD - 2, PAD + 22), 22, t.orange, 'semibold', 2)
    row, top = 27, PAD + 28
    order = [i for i in items if not i[1]] + [i for i in items if i[1]]
    for k, (text, done) in enumerate(order[:max(1, int((H - PAD - top) / row)) - 1]):
        y = top + row * k
        cx, cy = PAD + 9, y + row / 2
        if done:
            c.circle(cx, cy, 8, t.orange)
            c.circle(cx, cy, 3.2, rgba(0xFFFFFF, 0.98))
        else:
            c.arc(cx, cy, 7.4, -90, 270, t.sub, 1.5, caps=False)
        c.text(text, (PAD + 26, y, W - PAD, y + row), 13.5, t.sub if done else t.text)
    # The last row adds one.
    shown = min(len(order), max(1, int((H - PAD - top) / row)) - 1)
    if shown < int((H - PAD - top) / row):
        y = top + row * shown
        c.text('+', (PAD - 1, y, PAD + 19, y + row), 17, t.faint, 'regular', 1)
        c.text('新提醒', (PAD + 26, y, W - PAD, y + row), 13.5, t.faint)


def paint_notes(c, t, lines):
    """PaintNotes: the title in orange, then the text."""
    W, H = c.w, c.h
    c.text('备忘录', (PAD + 2, PAD - 2, W - PAD, PAD + 18), 13.5, t.orange, 'semibold')
    y = PAD + 24
    for line in lines:
        c.text(line, (PAD + 2, y, W - PAD - 2, y + 20), 13.5, t.text, valign='top')
        y += 20


def paint_calendar_large(c, t, y, m, d, lunar_today, labels, first=0):
    """The large calendar: the month, the lunar day under every date, today on a blue plate."""
    W, H = c.w, c.h
    c.text(MONTHS[m - 1], (PAD + 4, PAD, W / 2, PAD + 22), 16, t.blue, 'semibold')
    c.text(f'{y}年 · 农历{lunar_today}', (W / 2 - 40, PAD, W - PAD - 4, PAD + 22), 11.5, t.sub, 'regular', 2)
    gx, gw = PAD, W - 2 * PAD
    cw = gw / 7
    yy = PAD + 28
    for i in range(7):
        c.text(WEEK_SHORT[(first + i) % 7], (gx + i * cw, yy, gx + (i + 1) * cw, yy + 16), 10.5, t.sub, 'semibold', 1)
    yy += 20
    lead = (weekday(y, m, 1) - first + 7) % 7
    days = days_in(y, m)
    rows = (lead + days + 6) // 7
    rh = (H - PAD - yy) / max(rows, 5)
    for day in range(1, days + 1):
        cell = lead + day - 1
        x0, y0 = gx + (cell % 7) * cw, yy + (cell // 7) * rh
        cx = x0 + cw / 2
        today = day == d
        weekend = (first + cell % 7) % 7 >= 5
        if today:
            c.fill((cx - 18, y0 + 1, cx + 18, y0 + rh - 1), 10, t.blue)
        ink = rgba(0xFFFFFF, 0.98) if today else t.sub if weekend else t.text
        c.text(str(day), (x0, y0, x0 + cw, y0 + rh * 0.56), 14, ink, 'semibold', 1)
        label, festival = labels[day - 1]
        lc = alpha(rgba(0xFFFFFF, 0.98), 0.85) if today else t.blue if festival else t.sub
        c.text(label, (x0, y0 + rh * 0.48, x0 + cw, y0 + rh - 5), 9, lc, 'regular', 1)


def grip(c, t, left):
    """The resize grip (desktop_widgets Paint): an arc in the lower corner towards the screen's middle."""
    r = 22
    cx = r if left else c.w - r
    c.arc(cx, c.h - r, r - 6, 100 if left else 10, 170 if left else 80, t.sub, 3.5)


# ---------------------------------------------------------------- status ring

def wifi_fan(c, cx, cy, radius, lit, on, off):
    """DrawWifiFan: a wedge and two arcs, about 4:3, lit by the signal."""
    span, frm = 78, 270 - 39
    radii = [0.37, 0.595, 0.905]
    oy = cy + 0.5 * radius
    for k in range(3):
        col = on if lit >= k + 1 else off
        r = radii[k] * radius
        if k == 0:
            pts = [(cx, oy)]
            for i in range(25):
                a = math.radians(frm + span * i / 24)
                pts.append((cx + r * math.cos(a), oy + r * math.sin(a)))
            c.polygon(pts, col)
            # Its outline, round (0.19 * 0.3 of the radius): the corners soften.
            w = 0.19 * 0.3 * radius
            for (x0, y0), (x1, y1) in zip(pts, pts[1:] + pts[:1]):
                c.capsule(x0, y0, x1, y1, col, w)
        else:
            c.arc(cx, oy, r, frm, frm + span, col, 0.19 * radius)


def status_ring(c, cx, cy, h, ink, fill=0.78, charging=False, mode='battery', low=False, signal=3, volume=70):
    """DrawStatus: the open ring (64 deg gap at the bottom) filled clockwise from the gap's left end."""
    dim = lambda a: alpha(ink, a)
    gap = 64
    start, sweep = 90 + gap / 2, 360 - gap
    rad, stroke = h * 0.44, max(1.5, h * 0.07)
    c.arc(cx, cy, rad, start, start + sweep, dim(0.24), stroke)
    level = (rgba(0xE0352B) if ink[0] < 128 else rgba(0xFF453A)) if low else ink
    c.arc(cx, cy, rad, start, start + sweep * max(0.0, min(1.0, fill)), level, stroke)
    if charging and mode == 'battery':
        pts = [(0.18, -0.5), (-0.4, 0.08), (-0.02, 0.08), (-0.18, 0.5), (0.4, -0.08), (0.02, -0.08)]
        c.polygon([(cx + p[0] * 0.17 * h, cy + (0.44 + p[1] * 0.26) * h) for p in pts], ink)
    if mode == 'battery':
        size = h * 0.6
        wifi_fan(c, cx, cy, size * 0.62, signal, ink, dim(0.24))
    else:
        g = {'volume': '' if volume >= 67 else '' if volume >= 34 else '', 'mute': '', 'brightness': ''}[mode]
        c.glyph(g, (cx - h / 2, cy - h / 2, cx + h / 2, cy + h / 2), h * 0.40, ink, halo=False)


# ---------------------------------------------------------------- app icons

def _grad(size, top, bottom):
    t = np.linspace(0, 1, size, dtype=np.float32)[:, None, None]
    a = np.array(rgba(top)[:3], np.float32)
    b = np.array(rgba(bottom)[:3], np.float32)
    rows = a + (b - a) * t
    return np.broadcast_to(rows, (size, size, 3))


def _tile(size, top, bottom):
    """A rounded square tile with a vertical gradient and a lit top edge."""
    rgb = _grad(size, top, bottom)
    mask = Image.new('L', (size, size), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, size - 1, size - 1), size * 0.225, fill=255)
    img = Image.fromarray(rgb.astype(np.uint8), 'RGB').convert('RGBA')
    img.putalpha(mask)
    rim = Image.new('L', (size, size), 0)
    ImageDraw.Draw(rim).rounded_rectangle((0, 0, size - 1, size - 1), size * 0.225, outline=70, width=max(1, size // 80))
    rim = Image.fromarray((np.asarray(rim, np.float32) * np.linspace(1, 0.1, size)[:, None]).astype(np.uint8))
    white = Image.new('RGBA', (size, size), (255, 255, 255, 0))
    white.putalpha(rim)
    img.alpha_composite(white)
    return img


def _paint(img, colour, fn):
    mask = Image.new('L', img.size, 0)
    fn(ImageDraw.Draw(mask))
    layer = Image.new('RGBA', img.size, tuple(colour[:3]) + (0,))
    layer.putalpha(mask.point(lambda v: v * colour[3] // 255))
    img.alpha_composite(layer)


def app_icon(kind, size=384):
    S = size
    u = lambda v: v * S
    if kind == 'files':
        img = _tile(S, 0xFFD66B, 0xF2A019)
        _paint(img, rgba(0xFFFFFF, 0.55), lambda d: d.rounded_rectangle((u(.2), u(.27), u(.8), u(.74)), u(.06), fill=255))
        _paint(img, rgba(0xFFFFFF, 0.55), lambda d: d.rounded_rectangle((u(.2), u(.24), u(.46), u(.34)), u(.04), fill=255))
        _paint(img, rgba(0xFFFFFF, 1.0), lambda d: d.rounded_rectangle((u(.2), u(.36), u(.8), u(.76)), u(.06), fill=255))
    elif kind == 'browser':
        # A globe: meridians and parallels in ember on white.
        img = _tile(S, 0xFFFFFF, 0xEDE8E2)
        ink = rgba(0xE8562E)
        w = max(1, int(u(.04)))
        _paint(img, ink, lambda d: d.ellipse((u(.2), u(.2), u(.8), u(.8)), outline=255, width=w))
        _paint(img, ink, lambda d: d.ellipse((u(.36), u(.2), u(.64), u(.8)), outline=255, width=w))
        _paint(img, ink, lambda d: d.line((u(.5), u(.2), u(.5), u(.8)), fill=255, width=w))
        _paint(img, ink, lambda d: d.line((u(.2), u(.5), u(.8), u(.5)), fill=255, width=w))
        for y, half in ((.34, .245), (.66, .245)):
            _paint(img, ink, lambda d, y=y, half=half: d.line((u(.5 - half), u(y), u(.5 + half), u(y)), fill=255, width=w))
    elif kind == 'messages':
        img = _tile(S, 0x6EE38A, 0x1FAE4B)
        _paint(img, rgba(0xFFFFFF), lambda d: d.ellipse((u(.17), u(.22), u(.83), u(.72)), fill=255))
        _paint(img, rgba(0xFFFFFF), lambda d: d.polygon([(u(.30), u(.62)), (u(.22), u(.81)), (u(.45), u(.69))], fill=255))
    elif kind == 'music':
        img = _tile(S, 0xFF6A5F, 0xE3233D)
        _paint(img, rgba(0xFFFFFF), lambda d: d.polygon([(u(.40), u(.27)), (u(.70), u(.21)), (u(.70), u(.29)), (u(.40), u(.35))], fill=255))
        _paint(img, rgba(0xFFFFFF), lambda d: d.rectangle((u(.40), u(.28), u(.43), u(.66)), fill=255))
        _paint(img, rgba(0xFFFFFF), lambda d: d.rectangle((u(.67), u(.22), u(.70), u(.60)), fill=255))
        _paint(img, rgba(0xFFFFFF), lambda d: d.ellipse((u(.27), u(.59), u(.43), u(.72)), fill=255))
        _paint(img, rgba(0xFFFFFF), lambda d: d.ellipse((u(.54), u(.53), u(.70), u(.66)), fill=255))
    elif kind == 'photos':
        # A landscape at dusk: a sun over two hills.
        img = _tile(S, 0xFFD9A0, 0xF27A3D)
        _paint(img, rgba(0xFFF4E0), lambda d: d.ellipse((u(.56), u(.2), u(.78), u(.42)), fill=255))
        clip = Image.new('L', (S, S), 0)
        ImageDraw.Draw(clip).rounded_rectangle((0, 0, S - 1, S - 1), S * 0.225, fill=255)
        for pts, col in (([(0, .80), (.30, .48), (.62, .80)], 0xB8452A), ([(.28, .84), (.66, .52), (1.0, .84)], 0x7E2A1A)):
            hill = Image.new('L', (S, S), 0)
            ImageDraw.Draw(hill).polygon([(u(x), u(y)) for x, y in pts] + [(u(pts[-1][0]), S), (u(pts[0][0]), S)], fill=255)
            hill = ImageChops.multiply(hill, clip)
            layer = Image.new('RGBA', (S, S), tuple(rgba(col)[:3]) + (0,))
            layer.putalpha(hill)
            img.alpha_composite(layer)
    elif kind == 'notes':
        img = _tile(S, 0xFFFBEF, 0xF6EBCF)
        top = Image.new('L', (S, S), 0)
        ImageDraw.Draw(top).rounded_rectangle((0, 0, S - 1, S - 1), S * 0.225, fill=255)
        band = Image.new('L', (S, S), 0)
        ImageDraw.Draw(band).rectangle((0, 0, S, u(.27)), fill=255)
        band = ImageChops.multiply(band, top)
        layer = Image.new('RGBA', (S, S), (0xF8, 0xC4, 0x3A, 0))
        layer.putalpha(band)
        img.alpha_composite(layer)
        for i in range(4):
            y = u(.42 + i * .12)
            _paint(img, rgba(0xB8AE98, .9), lambda d, y=y: d.line((u(.16), y, u(.84), y), fill=255, width=max(1, int(u(.014)))))
    elif kind == 'terminal':
        img = _tile(S, 0x3C3C40, 0x1B1B1E)
        _paint(img, rgba(0xFFFFFF), lambda d: d.line([(u(.24), u(.36)), (u(.40), u(.50)), (u(.24), u(.64))], fill=255, width=int(u(.055)), joint='curve'))
        _paint(img, rgba(0xFFB35C), lambda d: d.rounded_rectangle((u(.46), u(.60), u(.74), u(.66)), u(.02), fill=255))
    elif kind == 'calendar':
        img = _tile(S, 0xFFFFFF, 0xF2EFEA)
        f1 = font(u(.17), 'semibold')
        f2 = font(u(.46), 'regular')
        _paint(img, rgba(0xF0453A), lambda d: d.text((u(.5), u(.22)), '周二', font=f1, fill=255, anchor='mm'))
        _paint(img, rgba(0x1C1C1E), lambda d: d.text((u(.5), u(.60)), '3', font=f2, fill=255, anchor='mm'))
    elif kind == 'settings':
        img = _tile(S, 0xDCDCE0, 0x9C9CA4)
        teeth = []
        for i in range(64):
            a = math.radians(i * 360 / 64)
            r = .30 if (i // 4) % 2 == 0 else .25
            teeth.append((u(.5 + r * math.cos(a)), u(.5 + r * math.sin(a))))
        _paint(img, rgba(0x3A3A3E), lambda d: d.polygon(teeth, fill=255))
        _paint(img, rgba(0xCFCFD4), lambda d: d.ellipse((u(.37), u(.37), u(.63), u(.63)), fill=255))
        _paint(img, rgba(0x3A3A3E), lambda d: d.ellipse((u(.45), u(.45), u(.55), u(.55)), fill=255))
    elif kind == 'code':
        img = _tile(S, 0x2E2E33, 0x131316)
        _paint(img, rgba(0xFF8A3D), lambda d: d.line([(u(.36), u(.32)), (u(.20), u(.50)), (u(.36), u(.68))], fill=255, width=int(u(.05)), joint='curve'))
        _paint(img, rgba(0xFF8A3D), lambda d: d.line([(u(.64), u(.32)), (u(.80), u(.50)), (u(.64), u(.68))], fill=255, width=int(u(.05)), joint='curve'))
        _paint(img, rgba(0xFFFFFF, .9), lambda d: d.line([(u(.56), u(.27)), (u(.44), u(.73))], fill=255, width=int(u(.045))))
    elif kind == 'mail':
        img = _tile(S, 0xFFFFFF, 0xEDE8E2)
        _paint(img, rgba(0xE8553A), lambda d: d.rounded_rectangle((u(.18), u(.28), u(.82), u(.72)), u(.05), fill=255))
        _paint(img, rgba(0xFFFFFF), lambda d: d.line([(u(.2), u(.32)), (u(.5), u(.53)), (u(.8), u(.32))], fill=255, width=int(u(.04)), joint='curve'))
    else:
        raise ValueError(kind)
    return img


def windows_logo(c, rect):
    """DrawLogo: four panes with Windows 11's light-to-deep blue."""
    l, t, r, b = rect
    w = min(r - l, b - t) * 0.80
    cx, cy = (l + r) / 2, (t + b) / 2
    left, top = cx - w / 2, cy - w / 2
    gap = w * 0.08
    pane = (w - gap) / 2
    corner = pane * 0.10
    S = 256
    yy, xx = np.mgrid[0:S, 0:S].astype(np.float32) / (S - 1)
    g = (xx + yy) / 2
    stops = [(0, 0x5CCBFF), (0.5, 0x1A9BF0), (1, 0x0060C8)]
    col = np.zeros((S, S, 3), np.float32)
    for (p0, c0), (p1, c1) in zip(stops, stops[1:]):
        m = (g >= p0) & (g <= p1)
        k = ((g - p0) / (p1 - p0))[..., None]
        a, b2 = np.array(rgba(c0)[:3], np.float32), np.array(rgba(c1)[:3], np.float32)
        col = np.where(m[..., None], a + (b2 - a) * k, col)
    grad = Image.fromarray(col.astype(np.uint8), 'RGB').convert('RGBA')
    mask = Image.new('L', (S, S), 0)
    md = ImageDraw.Draw(mask)
    sc = S / w
    for i in range(4):
        x, y = (i % 2) * (pane + gap), (i // 2) * (pane + gap)
        md.rounded_rectangle((x * sc, y * sc, (x + pane) * sc, (y + pane) * sc), corner * sc, fill=255)
    grad.putalpha(mask)
    sheen = Image.new('L', (S, S), 0)
    sd = ImageDraw.Draw(sheen)
    for i in range(4):
        x, y = (i % 2) * (pane + gap), (i // 2) * (pane + gap)
        sd.rectangle(((x + corner) * sc, y * sc, (x + pane - corner) * sc, (y + max(0.6, pane * 0.06)) * sc), fill=41)
    white = Image.new('RGBA', (S, S), (255, 255, 255, 0))
    white.putalpha(sheen)
    grad.alpha_composite(white)
    c.image(grad, left, top, w, w)
