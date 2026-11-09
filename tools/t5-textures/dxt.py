"""DXT1, DXT3 and DXT5 levels decoded with numpy, the same texels as TextureBuild::DecodeDxt, and one level of a DDS read
without the rest of the file.
"""
import numpy as np

import iwi


def _rgb565(v):
    r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1).astype(np.int32)


def decode(fmt, w, h, data):
    """RGBA uint8 (h, w, 4) of a level"""
    block = 8 if fmt == 11 else 16
    bw, bh = max(1, (w + 3) // 4), max(1, (h + 3) // 4)
    raw = np.frombuffer(data, np.uint8, bw * bh * block).reshape(bh * bw, block)
    colour = raw if fmt == 11 else raw[:, 8:]
    c0 = colour[:, 0].astype(np.int32) | (colour[:, 1].astype(np.int32) << 8)
    c1 = colour[:, 2].astype(np.int32) | (colour[:, 3].astype(np.int32) << 8)
    a, b = _rgb565(c0), _rgb565(c1)
    four = (c0 > c1) | (fmt != 11)
    palette = np.zeros((bw * bh, 4, 4), np.int32)
    palette[:, 0, :3], palette[:, 1, :3] = a, b
    palette[:, 2, :3] = np.where(four[:, None], (2 * a + b + 1) // 3, (a + b) // 2)
    palette[:, 3, :3] = np.where(four[:, None], (a + 2 * b + 1) // 3, 0)
    palette[:, :, 3] = 255
    palette[:, 3, 3] = np.where(four, 255, 0)
    bits = (colour[:, 4].astype(np.uint32) | (colour[:, 5].astype(np.uint32) << 8) |
            (colour[:, 6].astype(np.uint32) << 16) | (colour[:, 7].astype(np.uint32) << 24))
    indices = np.stack([(bits >> (2 * i)) & 3 for i in range(16)], 1).astype(np.int64)
    texels = np.take_along_axis(palette, indices[:, :, None].repeat(4, 2), 1)
    if fmt == 12:
        for i in range(16):
            texels[:, i, 3] = ((raw[:, i // 2] >> ((i % 2) * 4)) & 0xF).astype(np.int32) * 17
    elif fmt == 13:
        a0, a1 = raw[:, 0].astype(np.int32), raw[:, 1].astype(np.int32)
        alphas = np.zeros((bw * bh, 8), np.int32)
        alphas[:, 0], alphas[:, 1] = a0, a1
        seven = a0 > a1
        for i in range(1, 7):
            alphas[:, i + 1] = ((7 - i) * a0 + i * a1 + 3) // 7
        for i in range(1, 5):
            alphas[:, i + 1] = np.where(seven, alphas[:, i + 1], ((5 - i) * a0 + i * a1 + 2) // 5)
        alphas[:, 6] = np.where(seven, alphas[:, 6], 0)
        alphas[:, 7] = np.where(seven, alphas[:, 7], 255)
        wide = np.zeros(bw * bh, np.uint64)
        for i in range(6):
            wide |= raw[:, 2 + i].astype(np.uint64) << np.uint64(8 * i)
        picks = np.stack([(wide >> np.uint64(3 * i)) & np.uint64(7) for i in range(16)], 1).astype(np.int64)
        texels[:, :, 3] = np.take_along_axis(alphas, picks, 1)
    out = texels.reshape(bh, bw, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)
    return out[:h, :w].astype(np.uint8)


def read_dds_level(path, width, height):
    """the level of that size of a DDS as RGBA, None when it has none"""
    head = iwi.read_dds_header(path)
    if head is None:
        return None
    fmt, w, h, count = head
    at = 128
    for _ in range(count):
        size = iwi.level_bytes(fmt, w, h)
        if w == width and h == height:
            with open(path, 'rb') as f:
                f.seek(at)
                data = f.read(size)
            if fmt:
                return decode(fmt, w, h, data)
            return np.frombuffer(data, np.uint8).reshape(h, w, 4)[..., [2, 1, 0, 3]]
        at += size
        w, h = max(1, w // 2), max(1, h // 2)
    return None
