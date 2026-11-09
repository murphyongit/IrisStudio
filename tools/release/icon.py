# Draws the launcher's icon, launcher/res/IrisStudio.ico: the user's orb (2026-10-28), a circle shading from yellow in
# the middle through magenta to cyan at the rim, with nothing around it. The colors are the picture's, averaged around
# each radius. Each size is drawn on its own, 8x8 samples a pixel, so the small ones stay round.
# usage: python -I tools/release/icon.py [<out.ico>] [--png <folder>]   (--png also writes each size as a PNG)
import io
import os
import struct
import sys

import numpy as np
from PIL import Image

# (radius as a fraction of the circle's, color)
STOPS = [
    (0.00, 0xE9FD05), (0.05, 0xEAF50B), (0.10, 0xEBE718), (0.15, 0xEDD826), (0.20, 0xEDC637), (0.25, 0xEFB249),
    (0.30, 0xF19D5C), (0.35, 0xF38871), (0.40, 0xF47285), (0.45, 0xF75D99), (0.50, 0xF948AC), (0.55, 0xFB34BD),
    (0.60, 0xFB25CF), (0.65, 0xE42BE0), (0.70, 0xC63EEC), (0.75, 0xA45CF4), (0.80, 0x7C81F8), (0.85, 0x56A8FA),
    (0.90, 0x33CBFC), (0.95, 0x15E8FE), (1.00, 0x00FFFF),
]
SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
SAMPLES = 8


def orb(size):
    n = size * SAMPLES
    centers = (np.arange(n) + 0.5) / n * 2.0 - 1.0
    x, y = np.meshgrid(centers, centers)
    r = np.sqrt(x * x + y * y)
    at = [s[0] for s in STOPS]
    channels = [np.interp(np.minimum(r, 1.0), at, [(s[1] >> shift) & 0xFF for s in STOPS]) for shift in (16, 8, 0)]
    inside = (r <= 1.0).astype(np.float64)
    # the average of each pixel's samples: the color over the samples inside, the share inside as alpha
    blocks = lambda a: a.reshape(size, SAMPLES, size, SAMPLES).sum(axis=(1, 3))
    count = blocks(inside)
    rgb = [np.where(count > 0, blocks(c * inside) / np.maximum(count, 1), 0) for c in channels]
    alpha = count / (SAMPLES * SAMPLES) * 255
    pixels = np.dstack(rgb + [alpha]).round().clip(0, 255).astype(np.uint8)
    return Image.fromarray(pixels, 'RGBA')


def write_ico(path, images):
    # an icon directory of PNG images (Windows Vista and later read PNG entries of any size)
    entries = []
    for image in images:
        data = io.BytesIO()
        image.save(data, 'PNG', optimize=True)
        entries.append((image.width, data.getvalue()))
    offset = 6 + 16 * len(entries)
    out = struct.pack('<HHH', 0, 1, len(entries))
    for size, data in entries:
        out += struct.pack('<BBBBHHII', size % 256, size % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    with open(path, 'wb') as f:
        f.write(out + b''.join(data for _, data in entries))


def main():
    args = sys.argv[1:]
    png = None
    if '--png' in args:
        png = args[args.index('--png') + 1]
        del args[args.index('--png'):args.index('--png') + 2]
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    out = args[0] if args else os.path.join(root, 'launcher', 'res', 'IrisStudio.ico')
    images = [orb(size) for size in SIZES]
    write_ico(out, images)
    print(f'{out}: {len(images)} sizes, {os.path.getsize(out)} bytes')
    if png:
        os.makedirs(png, exist_ok=True)
        for image in images:
            image.save(os.path.join(png, f'icon-{image.width}.png'))


if __name__ == '__main__':
    main()
