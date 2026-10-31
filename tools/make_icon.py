"""Generates assets/atrium.ico (and a 256 px PNG) without third-party packages.

The mark: a gradient disc with a frosted glass bar across its lower half.
Usage: python tools/make_icon.py
"""

import math
import os
import struct
import zlib

SIZES = [16, 20, 24, 32, 40, 48, 64, 256]
SS = 4  # supersampling per axis


def lerp(a, b, t):
    return a + (b - a) * t


def sd_round_rect(px, py, cx, cy, hw, hh, r):
    qx = abs(px - cx) - hw + r
    qy = abs(py - cy) - hh + r
    return math.hypot(max(qx, 0), max(qy, 0)) + min(max(qx, qy), 0) - r


def shade(u, v):
    """Premultiplied RGBA in 0..1 for a point in the unit square."""
    d = math.hypot(u - 0.5, v - 0.5)
    if d > 0.47:
        return (0, 0, 0, 0)
    t = (u + v) / 2
    r, g, b = lerp(0.43, 0.55, t), lerp(0.70, 0.36, t), lerp(1.00, 0.98, t)
    # soft top-left sheen on the disc
    sheen = max(0.0, 1 - math.hypot(u - 0.34, v - 0.26) / 0.38) ** 2 * 0.28
    r, g, b = r + sheen, g + sheen, b + sheen
    a = 1.0
    # frosted bar
    bar = sd_round_rect(u, v, 0.5, 0.64, 0.30, 0.095, 0.095)
    if bar < 0:
        depth = min(1.0, -bar / 0.03)
        fr, fg, fb = 0.97, 0.98, 1.0
        mix = 0.78
        r, g, b = lerp(r, fr, mix), lerp(g, fg, mix), lerp(b, fb, mix)
        rim = 1 - depth
        r, g, b = r + rim * 0.15, g + rim * 0.15, b + rim * 0.15
    elif bar < 0.025:
        k = 1 - bar / 0.025
        r, g, b = r * (1 - 0.18 * k), g * (1 - 0.18 * k), b * (1 - 0.18 * k)
    r, g, b = min(r, 1), min(g, 1), min(b, 1)
    return (r * a, g * a, b * a, a)


def render(size):
    rows = []
    for y in range(size):
        row = bytearray()
        for x in range(size):
            acc = [0.0, 0.0, 0.0, 0.0]
            for sy in range(SS):
                for sx in range(SS):
                    c = shade((x + (sx + 0.5) / SS) / size, (y + (sy + 0.5) / SS) / size)
                    for i in range(4):
                        acc[i] += c[i]
            n = SS * SS
            a = acc[3] / n
            if a <= 0:
                row += b"\0\0\0\0"
                continue
            # un-premultiply for PNG
            row += bytes([round(min(1, acc[0] / n / a) * 255), round(min(1, acc[1] / n / a) * 255),
                          round(min(1, acc[2] / n / a) * 255), round(a * 255)])
        rows.append(bytes(row))
    return rows


def png(size, rows):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    raw = b"".join(b"\0" + r for r in rows)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets")
    os.makedirs(root, exist_ok=True)
    images = [(s, png(s, render(s))) for s in SIZES]
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries, blobs = b"", b""
    for s, data in images:
        dim = 0 if s >= 256 else s
        entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset + len(blobs))
        blobs += data
    with open(os.path.join(root, "atrium.ico"), "wb") as f:
        f.write(header + entries + blobs)
    with open(os.path.join(root, "atrium-256.png"), "wb") as f:
        f.write(images[-1][1])
    print("wrote assets/atrium.ico and assets/atrium-256.png")


if __name__ == "__main__":
    main()
