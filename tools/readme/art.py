"""Renders the README artwork into docs/images/readme/.

    python3 tools/readme/art.py [hero|material|ring|widgets|all]

Needs numpy, scipy and Pillow (with raqm); the MiSans files come from
assets/fonts, the symbols from Windows' Segoe Fluent Icons.
"""

import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(__file__))

import backdrop  # noqa: E402
import faces  # noqa: E402
import glass  # noqa: E402
from draw import ROOT, Canvas, Theme, alpha, over, rgba, to_image  # noqa: E402
from scene import Bar, widget  # noqa: E402

OUT = os.path.join(ROOT, 'docs', 'images', 'readme')
S = 2.0  # px per DIP: a 200 % display

APPS = ['files', 'browser', 'mail', 'messages', 'music', 'photos', 'notes', 'calendar', 'code', 'terminal', 'settings']

COPY = {
    'zh': {
        'eyebrow': 'ATRIUM DESKTOP',
        'title': ['桌面，', '换一种材质。'],
        'body': ['任务栏、小组件、通知与控制中心，', '共用同一块实时折射的液态玻璃。'],
        'foot': '原生 C++20 · Direct3D 11 · DirectComposition · Windows 11',
    },
    'en': {
        'eyebrow': 'ATRIUM DESKTOP',
        'title': ['Your desktop,', 'in glass.'],
        'body': ['A taskbar, widgets, notifications and a control centre,', 'all one liquid glass that refracts in real time.'],
        'foot': 'Native C++20 · Direct3D 11 · DirectComposition · Windows 11',
    },
}


def save(img, name):
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, name)
    im = to_image(img)
    if name.endswith('.jpg'):
        im.save(path, quality=92, subsampling=0, optimize=True, progressive=True)
    else:
        im.save(path, optimize=True)
    print(path, os.path.getsize(path) // 1024, 'KB')


def headline(img, lang, x, y, s):
    """The type block over the dark part of the backdrop (no glass: no halo)."""
    cp = COPY[lang]
    W = 620
    c = Canvas(W, 330, s)
    c.text(cp['eyebrow'], (0, 0, W, 18), 12.5, rgba(0xFFB36B, 0.95), 'semibold', 0, glass=False, tracking=0.16)
    ty = 34
    size = 62 if lang == 'zh' else 64
    for line in cp['title']:
        c.text(line, (0, ty, W, ty + size * 1.18), size, rgba(0xFFFFFF, 0.97), 'semibold', 0, glass=False, features=('kern',))
        ty += size * 1.16
    ty += 18
    for line in cp['body']:
        c.text(line, (0, ty, W, ty + 26), 18.5, rgba(0xFFFFFF, 0.64), 'regular', 0, glass=False)
        ty += 28
    over(img, c.result(), x * s, y * s)


def hero(lang):
    Wd, Hd = 1200, 680
    W, H = int(Wd * S), int(Hd * S)
    bg = backdrop.ember(W, H)
    bd = glass.Backdrop(bg)
    img = bg.copy()

    # The widgets: the right half of the grid (cell 164, gap 16, margin 24), anchored right.
    colA, colB = Wd - 24 - 164, Wd - 24 - 164 - 16 - 164
    rows = [24, 204, 384]
    widget(img, bd, colB, rows[0], 344, 164, lambda c, t: faces.paint_calendar_medium(c, t, 2026, 11, 3, '九月廿五'), S)
    widget(img, bd, colB, rows[1], 164, 164, lambda c, t: faces.paint_clock_small(c, t, (10, 9, 32)), S)
    widget(img, bd, colA, rows[1], 164, 164, lambda c, t: faces.paint_battery(c, t, 82, True), S)
    widget(img, bd, colB, rows[2], 344, 164, lambda c, t: faces.paint_system_medium(
        c, t, [('CPU', 0.23, '处理器'), ('内存', 0.58, '9.2 / 16 GB'), ('磁盘', 0.74, '214 GB 可用')]), S)

    # The bar, the pointer over the photos: the wave of the magnification.
    bar = Bar(APPS, pointer=None, running={'browser', 'files', 'code', 'music', 'terminal'}, active='code',
              status=dict(fill=0.82, charging=True, signal=3), s=S)
    bar.pointer = bar.app_x(APPS.index('photos')) + 6
    bar.draw(img, bd, Wd / 2, Hd - 10)

    headline(img, lang, 64, 92, S)
    save(img, f'hero-{lang}.jpg')


LUNAR_NOV_2026 = ['廿三', '廿四', '廿五', '廿六', '廿七', '廿八', '廿九', '三十', '十月'] + \
    ['初二', '初三', '初四', '初五', '初六', '初七', '初八', '初九', '初十', '十一', '十二', '十三', '十四', '十五', '十六', '十七', '十八',
     '十九', '二十', '廿一', '廿二']

TEXT = {
    'zh': {
        'liquid': ('液态', '折射 30 · 边缘 24 · 色散 0.30 · 模糊 0.50'),
        'frosted': ('磨砂', '折射 6 · 边缘 10 · 色散 0.04 · 模糊 1.00'),
        'states': [('电量与 Wi-Fi', '82% · 信号三格'), ('充电', '缺口里一道闪电'), ('音量', '变化时显示 1.5 秒'),
                   ('亮度', '亮度键与滑块'), ('低电量', '未充电且 ≤ 20%')],
    },
    'en': {
        'liquid': ('Liquid', 'refraction 30 · edge 24 · dispersion 0.30 · blur 0.50'),
        'frosted': ('Frosted', 'refraction 6 · edge 10 · dispersion 0.04 · blur 1.00'),
        'states': [('Battery, Wi-Fi', '82 % · three bars'), ('Charging', 'a bolt in the gap'), ('Volume', 'shown for 1.5 s'),
                   ('Brightness', 'keys and slider'), ('Low battery', '≤ 20 %, not charging')],
    },
}


def material(lang):
    """The two presets side by side over the same ribbons."""
    Wd, Hd = 1200, 520
    bg = backdrop.strata(int(Wd * S), int(Hd * S))
    bd = glass.Backdrop(bg)
    img = bg.copy()
    tx = TEXT[lang]
    for (x, mat, key) in ((56, glass.LIQUID, 'liquid'), (632, glass.FROSTED, 'frosted')):
        w, h, y = 512, 300, 110

        def paint(c, t, key=key):
            name, params = tx[key]
            c.text(name, (0, h / 2 - 44, w, h / 2 + 6), 40, t.text, 'semibold', 1)
            c.text(params, (0, h / 2 + 18, w, h / 2 + 42), 14, t.sub, 'regular', 1)
        widget(img, bd, x, y, w, h, paint, S, mat, radius=44)
    save(img, f'material-{lang}.jpg')


def ring(lang):
    """The status ring's states, each on a round piece of the bar's glass."""
    Wd, Hd = 1200, 380
    bg = backdrop.band(int(Wd * S), int(Hd * S))
    bd = glass.Backdrop(bg)
    img = bg.copy()
    states = [dict(fill=0.82, signal=3), dict(fill=0.64, charging=True, signal=3), dict(fill=0.60, mode='volume', volume=60),
              dict(fill=0.45, mode='brightness'), dict(fill=0.15, low=True, signal=2)]
    D = 150
    gap = (Wd - 5 * D) / 6
    for i, st in enumerate(states):
        x = gap + i * (D + gap)
        y = 64

        def paint(c, t, st=st):
            faces.status_ring(c, D / 2, D / 2, D * 0.56, t.text, **st)
        widget(img, bd, x, y, D, D, paint, S, glass.LIQUID, radius=D / 2)
        c = Canvas(D + gap, 60, S)
        name, detail = TEXT[lang]['states'][i]
        c.text(name, (0, 0, D + gap, 24), 16, rgba(0xFFFFFF, 0.95), 'semibold', 1, glass=False)
        c.text(detail, (0, 26, D + gap, 48), 13, rgba(0xFFFFFF, 0.58), 'regular', 1, glass=False)
        over(img, c.result(), (x - gap / 2) * S, (y + D + 34) * S)
    save(img, f'ring-{lang}.jpg')


def cursor(c, x, y):
    """Windows' diagonal resize cursor (IDC_SIZENWSE) at (x, y)."""
    import math
    pts = [(-11, 0), (-5, -5.5), (-5, -1.6), (5, -1.6), (5, -5.5), (11, 0), (5, 5.5), (5, 1.6), (-5, 1.6), (-5, 5.5)]
    a = math.radians(45)
    rot = [(x + px * math.cos(a) - py * math.sin(a), y + px * math.sin(a) + py * math.cos(a)) for px, py in pts]
    for p, q in zip(rot, rot[1:] + rot[:1]):
        c.capsule(p[0], p[1], q[0], q[1], rgba(0x000000, 0.95), 2.2)
    c.polygon(rot, rgba(0xFFFFFF))


def widgets(lang):
    """Both halves of the widget grid, on a light desktop: the ink turns dark."""
    Wd, Hd = 1040, 572
    bg = backdrop.dune(int(Wd * S), int(Hd * S))
    bd = glass.Backdrop(bg)
    img = bg.copy()
    left, top, cell, g = 24, 24, 164, 16
    widget(img, bd, left, top, 344, 164, lambda c, t: faces.paint_world_clock(c, t, [
        ('北京', '今天 +0小时', (10, 9, 32)), ('东京', '今天 +1小时', (11, 9, 32)), ('伦敦', '今天 -8小时', (2, 9, 32)), ('纽约', '昨天 -13小时', (21, 9, 32))]), S)
    widget(img, bd, left, top + cell + g, 164, 164, lambda c, t: faces.paint_digital(c, t, '10:09', '星期二', '11月3日'), S)
    notes_x = left + cell + g

    def notes(c, t):
        faces.paint_notes(c, t, ['周五前交设计评审', '换上新的壁纸', '试试磨砂预设'])
        faces.grip(c, t, left=False)
    widget(img, bd, notes_x, top + cell + g, 164, 164, notes, S)
    widget(img, bd, left, top + 2 * (cell + g), 344, 164, lambda c, t: faces.paint_reminders(c, t, [
        ('给 Atrium 拍一组截图', False), ('在稳定版 Windows 11 上测试', False), ('多显示器', False), ('整理验证记录', True)]), S)
    colB = Wd - 24 - 344
    labels = [(l, False) for l in LUNAR_NOV_2026]
    widget(img, bd, colB, top, 344, 344, lambda c, t: faces.paint_calendar_large(c, t, 2026, 11, 3, '九月廿五', labels), S)
    # The photo: a crop of the hero's backdrop, cut to the widget's corners.
    photo = backdrop.ember(int(344 * S * 1.6), int(164 * S * 1.6))
    from PIL import Image as _I, ImageDraw as _D
    ph = to_image(photo).resize((int(344 * S), int(164 * S)), _I.LANCZOS).convert('RGBA')
    mask = _I.new('L', (ph.width * 4, ph.height * 4), 0)
    _D.Draw(mask).rounded_rectangle((0, 0, mask.width - 1, mask.height - 1), 22 * S * 4, fill=255)
    ph.putalpha(mask.resize(ph.size, _I.LANCZOS))
    glass.render(img, bd, (colB * S, (top + 2 * (cell + g)) * S, (colB + 344) * S, (top + 2 * (cell + g) + 164) * S), 22 * S, glass.LIQUID, S)
    over(img, ph, colB * S, (top + 2 * (cell + g)) * S)
    # The pointer on the notes' grip.
    c = Canvas(60, 60, S)
    cursor(c, 30, 30)
    over(img, c.result(), (notes_x + 164 - 30 - 12) * S, (top + cell + g + 164 - 30 - 12) * S)
    save(img, f'widgets-{lang}.jpg')


def bar(lang):
    """The bar alone, close up, the pointer between two icons."""
    Wd, Hd = 1200, 190
    bg = backdrop.strata(int(Wd * S), int(Hd * S * 2.4))[int(Hd * S * 0.9):int(Hd * S * 0.9) + int(Hd * S)]
    bd = glass.Backdrop(np.ascontiguousarray(bg))
    img = bd.img.copy()
    apps = APPS[:9]
    b = Bar(apps, running={'browser', 'files', 'code', 'music'}, active='browser', status=dict(fill=0.82, signal=3), s=S)
    b.pointer = b.app_x(apps.index('messages')) + 14
    b.draw(img, bd, Wd / 2, Hd - 40)
    save(img, f'bar-{lang}.jpg')


if __name__ == '__main__':
    what = sys.argv[1] if len(sys.argv) > 1 else 'all'
    if what in ('hero', 'all'):
        for lang in ('zh', 'en'):
            hero(lang)
    for name, fn in (('material', material), ('ring', ring), ('widgets', widgets), ('bar', bar)):
        if what in (name, 'all'):
            for lang in ('zh', 'en'):
                fn(lang)
