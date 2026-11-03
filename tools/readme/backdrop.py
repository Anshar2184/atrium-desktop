"""An original backdrop for the artwork: ribbons of lit silk over warm dark.

Glass shows what is behind it, so the backdrop has bright, sharp-edged
ribbons for the lens to bend and plain dark for type to sit on. The palette
stays warm (ember, amber, champagne, a deep sage) on purpose.
"""

from dataclasses import dataclass, field
import math

import numpy as np
from scipy import ndimage


def hexrgb(h):
    return np.array([(h >> 16) & 255, (h >> 8) & 255, h & 255], np.float32) / 255


@dataclass
class Ribbon:
    y: float                 # centre height at u = 0 (in screen heights)
    slope: float             # rise per screen height across
    width: float             # half width, in screen heights
    top: int                 # colour at the lit edge
    bottom: int              # colour at the far edge
    waves: list = field(default_factory=list)   # (amplitude, frequency, phase) of the centre line
    swell: tuple = (0.3, 0.6, 0.0)              # width modulation (amount, frequency, phase)
    folds: tuple = (1.3, 0.0, 0.55)             # light across the silk (frequency, phase, strength)
    sheen: float = 0.75                          # the specular line near the lit edge
    soft: float = 0.10                           # feather of the far edge, in half widths
    shade: float = 0.55                          # the shadow it casts below


def silk(W, H, ribbons, base_top=0x0E0C0B, base_bottom=0x17110D, glow=None, seed=7, vignette=0.38, bloom=0.55):
    y, x = np.mgrid[0:H, 0:W].astype(np.float32)
    u, v = x / H, y / H
    top, bottom = hexrgb(base_top), hexrgb(base_bottom)
    img = top + (bottom - top) * (v / v.max())[..., None]
    if glow:  # a wide warm light behind everything: (u, v, radius, colour, strength)
        for gu, gv, gr, gc, gs in glow:
            g = np.exp(-((u - gu) ** 2 + (v - gv) ** 2) / (gr * gr))[..., None]
            img = img + hexrgb(gc) * g * gs
    px = 1.0 / H
    for rb in ribbons:
        c = rb.y + rb.slope * u
        for a, f, p in rb.waves:
            c = c + a * np.sin(2 * math.pi * (f * u + p))
        sa, sf, sp = rb.swell
        w = rb.width * (1 + sa * np.sin(2 * math.pi * (sf * u + sp)))
        d = (v - c) / w  # -1 at the lit edge, +1 at the far edge
        # Its shadow on what is below, then the ribbon.
        below = np.clip(d - 1, 0, None)
        img = img * (1 - rb.shade * np.exp(-below / 0.35) * (d > 1))[..., None]
        edge_top = np.clip((d + 1) * w / (1.3 * px), 0, 1)
        edge_far = np.clip((1 - d) / rb.soft, 0, 1)
        alpha = (edge_top * edge_far)[..., None]
        t = np.clip((d + 1) / 2, 0, 1)[..., None]
        colour = hexrgb(rb.top) + (hexrgb(rb.bottom) - hexrgb(rb.top)) * t ** 0.85
        ff, fp, fs = rb.folds
        fold = 0.5 + 0.5 * np.sin(2 * math.pi * (ff * u + fp) + 2.2 * d)
        light = (1 - fs) + fs * 1.35 * fold ** 1.6
        spec = rb.sheen * np.exp(-((d + 0.80) / 0.075) ** 2) * fold
        colour = colour * light[..., None] + (spec * 0.9)[..., None] * np.array([1.0, 0.95, 0.88], np.float32)
        img = img * (1 - alpha) + colour * alpha
    # A soft bloom of the brightest parts, a vignette, and grain against banding.
    bright = np.clip(img - 0.55, 0, None)
    glow_img = np.stack([ndimage.gaussian_filter(bright[..., k], H * 0.03) for k in range(3)], -1)
    img = img + glow_img * bloom
    cu, cv = W / H / 2, 0.5
    vig = 1 - vignette * np.clip(((u - cu) / (W / H * 0.62)) ** 2 + ((v - cv) / 0.75) ** 2, 0, 1)
    img = img * vig[..., None]
    rng = np.random.default_rng(seed)
    img = img + (rng.standard_normal((H, W, 1)).astype(np.float32) * (1.4 / 255))
    return np.clip(img, 0, 1)


def ember(W, H):
    """The hero's backdrop: ribbons rising from the lower left to the upper right."""
    rs = [
        Ribbon(1.02, -0.36, 0.20, 0x5A1C10, 0x1C0B08, [(0.05, 0.42, 0.10)], (0.25, 0.5, 0.2), (0.9, 0.3, 0.5), 0.35, 0.25, 0.45),
        Ribbon(1.10, -0.40, 0.13, 0xE2572A, 0x7A1D12, [(0.06, 0.38, 0.30)], (0.35, 0.7, 0.6), (1.1, 0.1, 0.6), 0.85, 0.12, 0.55),
        Ribbon(1.24, -0.42, 0.085, 0xFFC27A, 0xE0702E, [(0.05, 0.45, 0.42)], (0.40, 0.8, 0.1), (1.3, 0.6, 0.55), 1.0, 0.10, 0.55),
        Ribbon(1.36, -0.40, 0.06, 0xF8EAD2, 0xC89B6A, [(0.045, 0.50, 0.55)], (0.45, 0.9, 0.4), (1.4, 0.2, 0.5), 1.0, 0.10, 0.50),
        Ribbon(1.50, -0.37, 0.10, 0x3F6B5C, 0x14251F, [(0.04, 0.40, 0.65)], (0.30, 0.6, 0.8), (1.0, 0.5, 0.5), 0.6, 0.15, 0.55),
        Ribbon(1.66, -0.36, 0.09, 0xD9512A, 0x5C160D, [(0.05, 0.36, 0.80)], (0.30, 0.6, 0.3), (1.2, 0.4, 0.6), 0.9, 0.12, 0.5),
    ]
    glow = [(W / H * 0.72, 0.62, 0.55, 0x6B2A12, 0.35), (W / H * 0.15, 1.05, 0.45, 0x4A1A0C, 0.30)]
    return silk(W, H, rs, glow=glow)



def dune(W, H):
    """A light backdrop (dark ink on the glass): sand, terracotta and sage over cream."""
    rs = [
        Ribbon(0.30, 0.10, 0.16, 0xF3E2C8, 0xD9BE98, [(0.05, 0.40, 0.20)], (0.25, 0.5, 0.1), (0.9, 0.2, 0.35), 0.5, 0.25, 0.22),
        Ribbon(0.52, 0.06, 0.10, 0xE07A4A, 0x9A3A1F, [(0.06, 0.45, 0.45)], (0.35, 0.7, 0.3), (1.1, 0.6, 0.45), 0.9, 0.12, 0.30),
        Ribbon(0.70, 0.02, 0.075, 0xFFF8EC, 0xE3D0B4, [(0.05, 0.50, 0.70)], (0.40, 0.8, 0.6), (1.3, 0.3, 0.40), 1.0, 0.10, 0.28),
        Ribbon(0.86, 0.05, 0.09, 0x9FBEA6, 0x4E7660, [(0.05, 0.38, 0.05)], (0.30, 0.6, 0.9), (1.0, 0.8, 0.45), 0.7, 0.14, 0.30),
        Ribbon(1.04, 0.03, 0.10, 0xE9A15C, 0xB4602C, [(0.04, 0.42, 0.35)], (0.30, 0.6, 0.5), (1.2, 0.1, 0.45), 0.9, 0.12, 0.28),
    ]
    return silk(W, H, rs, base_top=0xEEE6DA, base_bottom=0xE3D6C6, glow=[(W / H * 0.5, 0.2, 0.6, 0xFFFFFF, 0.10)], vignette=0.12, bloom=0.25)


def strata(W, H):
    """Ribbons running across, for two pieces of glass side by side."""
    rs = [
        Ribbon(0.18, 0.05, 0.12, 0x5A1C10, 0x1C0B08, [(0.04, 0.35, 0.1)], (0.25, 0.5, 0.2), (0.9, 0.3, 0.5), 0.35, 0.25, 0.45),
        Ribbon(0.38, -0.03, 0.09, 0xE2572A, 0x7A1D12, [(0.07, 0.42, 0.30)], (0.35, 0.7, 0.6), (1.1, 0.1, 0.6), 0.85, 0.12, 0.55),
        Ribbon(0.55, 0.02, 0.06, 0xFFC27A, 0xE0702E, [(0.06, 0.48, 0.55)], (0.40, 0.8, 0.1), (1.3, 0.6, 0.55), 1.0, 0.10, 0.55),
        Ribbon(0.68, -0.02, 0.045, 0xF8EAD2, 0xC89B6A, [(0.05, 0.52, 0.75)], (0.45, 0.9, 0.4), (1.4, 0.2, 0.5), 1.0, 0.10, 0.50),
        Ribbon(0.84, 0.03, 0.08, 0x3F6B5C, 0x14251F, [(0.05, 0.40, 0.95)], (0.30, 0.6, 0.8), (1.0, 0.5, 0.5), 0.6, 0.15, 0.55),
    ]
    return silk(W, H, rs, glow=[(W / H * 0.5, 0.55, 0.7, 0x5A2410, 0.30)])



def band(W, H):
    """A narrow sheaf of ribbons across the upper half; dark below for captions."""
    rs = [
        Ribbon(0.16, 0.02, 0.10, 0x5A1C10, 0x1C0B08, [(0.03, 0.35, 0.1)], (0.25, 0.5, 0.2), (0.9, 0.3, 0.5), 0.35, 0.25, 0.45),
        Ribbon(0.26, -0.02, 0.07, 0xE2572A, 0x7A1D12, [(0.05, 0.42, 0.30)], (0.35, 0.7, 0.6), (1.1, 0.1, 0.6), 0.85, 0.12, 0.55),
        Ribbon(0.36, 0.01, 0.045, 0xFFC27A, 0xE0702E, [(0.045, 0.48, 0.55)], (0.40, 0.8, 0.1), (1.3, 0.6, 0.55), 1.0, 0.10, 0.55),
        Ribbon(0.44, -0.01, 0.03, 0xF8EAD2, 0xC89B6A, [(0.04, 0.52, 0.75)], (0.45, 0.9, 0.4), (1.4, 0.2, 0.5), 1.0, 0.10, 0.50),
        Ribbon(0.97, 0.02, 0.05, 0x3F6B5C, 0x14251F, [(0.03, 0.40, 0.95)], (0.30, 0.6, 0.8), (1.0, 0.5, 0.5), 0.6, 0.15, 0.55),
    ]
    return silk(W, H, rs, glow=[(W / H * 0.5, 0.35, 0.6, 0x5A2410, 0.30)])



def photo(name, W, H, focus=(0.5, 0.5), scale=1.0):
    """One of the settings' preview photographs (assets/backgrounds, Martin Martz / Unsplash),
    covering W x H: `scale` zooms in, `focus` places the crop (0..1 of the spare room)."""
    import os
    from PIL import Image
    path = os.path.join(os.path.dirname(__file__), '..', '..', 'assets', 'backgrounds', name + '.jpg')
    im = Image.open(path).convert('RGB')
    k = max(W / im.width, H / im.height) * scale
    w, h = int(round(im.width * k)), int(round(im.height * k))
    im = im.resize((w, h), Image.LANCZOS)
    x0, y0 = int((w - W) * focus[0]), int((h - H) * focus[1])
    return np.asarray(im.crop((x0, y0, x0 + W, y0 + H)), np.float32) / 255


if __name__ == '__main__':
    import sys
    from PIL import Image
    out = sys.argv[1] if len(sys.argv) > 1 else 'backdrop.png'
    img = ember(1200, 700)
    Image.fromarray((img * 255 + 0.5).astype(np.uint8)).save(out)
