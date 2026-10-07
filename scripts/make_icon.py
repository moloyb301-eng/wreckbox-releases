# Builds res/app_icon.ico (the window, taskbar and Explorer icon) from res/logo.svg, the logo in the README.
# The logo is a rounded tile with a gradient and square dots, so it is rasterised here directly (4×4 supersampling) and
# packed as PNG images in an .ico — no image library needed. Run: python scripts/make_icon.py
import re, struct, zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SVG = (ROOT / 'res' / 'logo.svg').read_text(encoding='utf-8')
SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
SS = 4  # samples per pixel per axis

def rgb(h):
    return tuple(int(h[i:i + 2], 16) for i in (1, 3, 5))

stops = [rgb(c) for c in re.findall(r'stop-color="(#[0-9A-Fa-f]{6})"', SVG)]
tile = re.search(r'<rect x="0" y="0" width="100" height="100" rx="([\d.]+)"', SVG)
radius = float(tile.group(1))
dots = [(float(x), float(y), float(w), float(h), rgb(c)) for x, y, w, h, c in
        re.findall(r'<rect x="([\d.]+)" y="([\d.]+)" width="([\d.]+)" height="([\d.]+)" fill="(#[0-9A-Fa-f]{6})"', SVG)]

def in_tile(x, y):  # the rounded square, in logo units (0..100)
    if not (0 <= x <= 100 and 0 <= y <= 100): return False
    cx = min(max(x, radius), 100 - radius)
    cy = min(max(y, radius), 100 - radius)
    return (x - cx) ** 2 + (y - cy) ** 2 <= radius ** 2

def render(n):
    # A grid of dot colours at sample resolution: later rects paint over earlier ones, as in the SVG.
    m = n * SS
    scale = 100 / m
    grid = [[None] * m for _ in range(m)]
    for x, y, w, h, c in dots:
        for j in range(max(0, int(y / scale)), min(m, int((y + h) / scale) + 1)):
            sy = (j + 0.5) * scale
            if not (y <= sy < y + h): continue
            for i in range(max(0, int(x / scale)), min(m, int((x + w) / scale) + 1)):
                sx = (i + 0.5) * scale
                if x <= sx < x + w: grid[j][i] = c
    rows = []
    for py in range(n):
        row = bytearray([0])  # PNG filter: none
        for px in range(n):
            r = g = b = a = 0
            for sj in range(SS):
                for si in range(SS):
                    i, j = px * SS + si, py * SS + sj
                    sx, sy = (i + 0.5) * scale, (j + 0.5) * scale
                    if not in_tile(sx, sy): continue
                    c = grid[j][i]
                    if c is None:  # the tile's diagonal gradient
                        t = (sx + sy) / 200
                        c = tuple(round(stops[0][k] + (stops[1][k] - stops[0][k]) * t) for k in range(3))
                    r, g, b, a = r + c[0], g + c[1], b + c[2], a + 1
            row += bytes([r // a, g // a, b // a, round(255 * a / SS ** 2)]) if a else bytes(4)
        rows.append(bytes(row))
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', n, n, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(b''.join(rows), 9)) + chunk(b'IEND', b''))

pngs = [render(n) for n in SIZES]
ico = struct.pack('<HHH', 0, 1, len(pngs))
offset = 6 + 16 * len(pngs)
for n, p in zip(SIZES, pngs):
    ico += struct.pack('<BBBBHHII', n % 256, n % 256, 0, 0, 1, 32, len(p), offset)
    offset += len(p)
(ROOT / 'res' / 'app_icon.ico').write_bytes(ico + b''.join(pngs))
print('res/app_icon.ico:', ', '.join(map(str, SIZES)), 'px')
