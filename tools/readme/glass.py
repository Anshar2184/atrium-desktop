"""Atrium's glass on the CPU: a port of native/core/render/shaders/glass.hlsl.

Every stage of the shader is kept (signed distance, circular lens profile,
per-channel dispersion, blurred backdrop, vibrancy, tint, rim light, sheen,
depth band, drop shadow), so the README artwork shows the same material as
the running bar. Images are float32 sRGB in 0..1, shape (H, W, 3), like the
shader's BGRA8 UNORM textures.
"""

from dataclasses import dataclass
import math

import numpy as np
from scipy import ndimage


@dataclass
class Material:
    """The settings' material keys (BarSettings / native settings.h), in DIP."""

    transparency: float = 0.95
    blur: float = 0.50
    refraction: float = 30
    edge: float = 24
    dispersion: float = 0.30
    highlight: float = 0.80
    shadow: float = 0.30
    tint: tuple = (1.0, 1.0, 1.0)


LIQUID = Material()
FROSTED = Material(transparency=0.80, blur=1.0, refraction=6, edge=10, dispersion=0.04, highlight=0.45, shadow=0.30,
                   tint=(0xF5 / 255, 0xF5 / 255, 0xFA / 255))

INSET_DIP = 14  # bar.cpp kInsetDip: room for the drop shadow
LIGHT = (-0.6, -0.8)  # ParamsFor: unit vector towards the light


def sd_round_rect(px, py, rect, r):
    l, t, rr, b = rect
    cx, cy = (l + rr) / 2, (t + b) / 2
    bx, by = max((rr - l) / 2, 1), max((b - t) / 2, 1)
    r = min(r, bx, by)
    qx = np.abs(px - cx) - bx + r
    qy = np.abs(py - cy) - by + r
    return np.hypot(np.maximum(qx, 0), np.maximum(qy, 0)) + np.minimum(np.maximum(qx, qy), 0) - r


class Backdrop:
    """The desktop behind the glass, with its blurred mips made once."""

    def __init__(self, img):
        self.img = img.astype(np.float32)
        self._mips = {}

    def blurred(self, lod):
        key = round(lod, 3)
        if key not in self._mips:
            # A mip at `lod` is a box of 2^lod px; bilinear sampling of it is close to this Gaussian.
            sigma = 0.45 * 2 ** lod
            self._mips[key] = np.stack([ndimage.gaussian_filter(self.img[..., c], sigma, mode='nearest') for c in range(3)], -1)
        return self._mips[key]


def _sample(img, x, y, channel=None):
    coords = [y - 0.5, x - 0.5]
    if channel is not None:
        return ndimage.map_coordinates(img[..., channel], coords, order=1, mode='nearest')
    return np.stack([ndimage.map_coordinates(img[..., c], coords, order=1, mode='nearest') for c in range(3)], -1)


def render(dst, backdrop, rect, radius, mat=LIQUID, scale=2.0):
    """Draws one piece of glass (rect in px, radius in px) into dst, in place.

    Returns the mean luminance under the glass, which picks the ink as the core
    does (dark ink above 0.50)."""
    H, W, _ = dst.shape
    inset = INSET_DIP * scale
    refract = mat.refraction * scale
    edge = max(mat.edge * scale, 1)
    blur_mix = min(1.0, mat.blur * 4.0)
    blur_lod = 1.0 + 4.5 * mat.blur
    tint_mix = 1.0 - mat.transparency
    hl = mat.highlight

    m = int(2 * inset + 2)
    x0, y0 = max(int(rect[0]) - m, 0), max(int(rect[1]) - m, 0)
    x1, y1 = min(int(math.ceil(rect[2])) + m, W), min(int(math.ceil(rect[3])) + m, H)
    yy, xx = np.mgrid[y0:y1, x0:x1].astype(np.float32)
    px, py = xx + 0.5, yy + 0.5
    dist = lambda X, Y: sd_round_rect(X, Y, rect, radius)

    d = dist(px, py)
    ds = dist(px, py - inset * 0.25)
    shadow_a = mat.shadow * np.exp(-np.maximum(ds, 0) / max(inset * 0.35, 1)) * (d >= 0)
    cov = np.clip(0.5 - d, 0, 1)

    gx = dist(px + 1, py) - dist(px - 1, py)
    gy = dist(px, py + 1) - dist(px, py - 1)
    gl = np.hypot(gx, gy)
    nx = np.where(gl > 1e-5, gx / np.maximum(gl, 1e-5), 0)
    ny = np.where(gl > 1e-5, gy / np.maximum(gl, 1e-5), 0)

    depth = np.maximum(-d, 0)
    t = np.clip(1 - depth / edge, 0, 1)
    profile = 1 - np.sqrt(np.clip(1 - t * t, 0, 1))  # circular lens cross-section
    ox, oy = -nx * refract * profile, -ny * refract * profile

    src = backdrop.img
    disp = mat.dispersion
    sharp = np.stack([
        _sample(src, px + ox * (1 + disp), py + oy * (1 + disp), 0),
        _sample(src, px + ox, py + oy, 1),
        _sample(src, px + ox * (1 - disp), py + oy * (1 - disp), 2),
    ], -1)
    mip = backdrop.blurred(blur_lod)
    s = 2 ** blur_lod * 0.75
    bx, by = px + ox, py + oy
    soft = _sample(mip, bx, by) * 0.4
    for dx, dy in ((s, 0), (-s, 0), (0, s), (0, -s)):
        soft += _sample(mip, bx + dx, by + dy) * 0.15
    col = sharp + (soft - sharp) * blur_mix

    # Vibrancy, then the tint.
    luma = (col @ np.array([0.2126, 0.7152, 0.0722], np.float32))[..., None]
    col = (luma + (col - luma) * (1.0 + 0.22 * hl)) * (1.0 + 0.04 * hl)
    col = col + (np.array(mat.tint, np.float32) - col) * tint_mix

    # Light: the rim line, the glow inside it, the sheen from the top.
    lx, ly = LIGHT
    lit = np.clip(nx * lx + ny * ly, 0, 1)
    back = np.clip(-(nx * lx + ny * ly), 0, 1)
    rim = 1 - _smoothstep(0.0, 1.6, depth)
    glow = np.clip(1 - depth / (edge * 1.6), 0, 1)
    light = hl * rim * (0.22 + 0.78 * lit + 0.35 * back) + hl * glow * glow * lit * 0.16
    sheen = np.clip(1 - (py - rect[1]) / max(rect[3] - rect[1], 1), 0, 1)
    light = light + hl * 0.045 * sheen * sheen
    col = col + light[..., None]
    band = _smoothstep(0.0, edge * 0.5, depth) * (1 - _smoothstep(edge * 0.5, edge * 1.3, depth))
    col = col * (1 - 0.05 * band * (1 - lit))[..., None]
    col = np.clip(col, 0, 1)

    region = dst[y0:y1, x0:x1]
    c = cov[..., None]
    region[:] = col * c + region * (1 - c) * (1 - shadow_a[..., None])
    inside = cov > 0.5
    under = col[inside] if inside.any() else col.reshape(-1, 3)
    return float((under @ np.array([0.2126, 0.7152, 0.0722], np.float32)).mean())


def _smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0, 1)
    return t * t * (3 - 2 * t)
