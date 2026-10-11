#!/usr/bin/env python3
"""Renders a filmstrip knob PNG (frames stacked vertically) with the Python stdlib only.

Used for examples/c/pluck.ui/damping.png:
    python3 examples/tools/gen_filmstrip.py examples/c/pluck.ui/damping.png --size 80 --frames 48
"""
import argparse
import math
import struct
import zlib


def png(path, width, height, rows):
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
    raw = b"".join(b"\0" + bytes(r) for r in rows)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def knob_pixel(x, y, s, n, leds):
    """RGBA of one pixel of a knob of size s at normalized value n (supersampled 3x3)."""
    acc = [0.0, 0.0, 0.0, 0.0]
    c = s / 2.0
    for sy in range(3):
        for sx in range(3):
            px, py = x + (sx + 0.5) / 3 - c, y + (sy + 0.5) / 3 - c
            r = math.hypot(px, py)
            col = None
            if r < c * 0.62:  # body: brushed dark cap with a soft top-left light
                t = max(0.0, min(1.0, 0.5 - (px + py) / (c * 1.6)))
                g = 40 + 60 * t
                col = (g, g * 0.97, g * 0.92, 255)
                ang = math.atan2(px, -py)
                a0 = math.radians(-135 + 270 * n)
                if abs((ang - a0 + math.pi) % (2 * math.pi) - math.pi) < 0.09 and c * 0.25 < r < c * 0.56:
                    col = (250, 240, 225, 255)  # pointer
            elif r < c * 0.66:
                col = (15, 14, 13, 255)
            else:  # LED ring
                ang = math.degrees(math.atan2(px, -py))
                if -140 <= ang <= 140 and c * 0.76 < r < c * 0.94:
                    i = round((ang + 135) / 270 * (leds - 1))
                    ca = -135 + 270 * i / (leds - 1)
                    d = math.hypot(px - math.sin(math.radians(ca)) * c * 0.85,
                                   py + math.cos(math.radians(ca)) * c * 0.85)
                    if d < c * 0.07:
                        lit = i <= round(n * (leds - 1)) and n > 0.001
                        col = (255, 170, 60, 255) if lit else (70, 50, 30, 255)
            if col:
                for k in range(4):
                    acc[k] += col[k] * (col[3] / 255.0 if k < 3 else 1.0)
    a = acc[3] / 9.0
    if a <= 0:
        return (0, 0, 0, 0)
    return tuple(int(min(255, acc[k] / 9.0 * 255.0 / a)) for k in range(3)) + (int(a),)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--size", type=int, default=80)
    ap.add_argument("--frames", type=int, default=48)
    ap.add_argument("--leds", type=int, default=15)
    a = ap.parse_args()
    rows = []
    for f in range(a.frames):
        n = f / (a.frames - 1)
        for y in range(a.size):
            row = []
            for x in range(a.size):
                row.extend(knob_pixel(x, y, a.size, n, a.leds))
            rows.append(row)
    png(a.out, a.size, a.size * a.frames, rows)


if __name__ == "__main__":
    main()
