"""Black Ops (T5) .iwi (version 13), .iwd and DDS: reading, writing back byte for byte, and the game's image search.

iwi13 header (48 bytes): 'IWi' 13, format (11 DXT1, 12 DXT3, 13 DXT5), flags (0x2 no mipmaps, 0x4 cube, 0x8 volume,
0x10 streaming), width, height, depth (u16 each), gamma (float: 2.0 colour, 0 normal maps), fileSizeForPicmip[8] (the
file's size with the levels from the smallest up to level i). The levels follow, smallest first. The same layout as
core/src/Components/TextureLogic.cpp.
"""
import glob
import os
import struct
import zipfile

FORMATS = {11: ('DXT1', 8), 12: ('DXT3', 16), 13: ('DXT5', 16)}
NO_MIPS, CUBE, VOLUME = 0x2, 0x4, 0x8


def search_folders(game, plutonium_storage=None):
    """the folders the game reads iwds from, the first searched first: Plutonium's storage\\t5\\main, then main"""
    folders = []
    if plutonium_storage:
        folders.append(os.path.join(plutonium_storage, 'main'))
    folders.append(os.path.join(game, 'main'))
    return [f for f in folders if os.path.isdir(f)]


def index(folders):
    """name -> (iwd path, entry, size, crc) for every images/*.iwi; the first folder wins, and in a folder the iwd
    named last (iw_43 over iw_42)"""
    found = {}
    for folder in reversed(folders):
        for path in sorted(glob.glob(os.path.join(folder, '*.iwd'))):
            with zipfile.ZipFile(path) as z:
                for info in z.infolist():
                    n = info.filename.lower()
                    if n.startswith('images/') and n.endswith('.iwi'):
                        found[n[7:-4]] = (path, info.filename, info.file_size, info.CRC)
    return found


def read(path, entry):
    with zipfile.ZipFile(path) as z:
        return z.read(entry)


def level_bytes(fmt, w, h):
    if fmt in FORMATS:
        return max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * FORMATS[fmt][1]
    return w * h * 4


def parse_header(data):
    if len(data) < 48 or data[:3] != b'IWi' or data[3] != 13:
        return None
    w, h, d = struct.unpack_from('<3H', data, 6)
    return dict(format=data[4], flags=data[5], width=w, height=h, depth=d, gamma=struct.unpack_from('<f', data, 12)[0],
                sizes=struct.unpack_from('<8I', data, 16))


def parse(data):
    """the header and, for DXT, the levels (w, h, bytes), largest first"""
    t = parse_header(data)
    if t is None:
        raise ValueError('not an iwi13')
    levels = []
    if t['format'] in FORMATS:
        lw, lh, end = t['width'], t['height'], len(data)
        while True:
            size = level_bytes(t['format'], lw, lh)
            levels.append((lw, lh, data[end - size:end]))
            end -= size
            if (lw == 1 and lh == 1) or t['flags'] & NO_MIPS:
                break
            lw, lh = max(1, lw // 2), max(1, lh // 2)
    t['levels'] = levels
    return t


def write_iwi(fmt, flags, gamma, levels, depth=1):
    """the file's bytes: levels largest first, written smallest first after the header"""
    body = b''.join(data for _, _, data in reversed(levels))
    sizes, size = [], 48 + len(body)
    for i in range(8):
        sizes.append(size)
        if i + 1 < len(levels):  # past the last level the smallest stays
            size -= len(levels[i][2])
    w, h = levels[0][0], levels[0][1]
    return b'IWi' + bytes([13, fmt, flags]) + struct.pack('<3Hf8I', w, h, depth, gamma, *sizes) + body


def write_dds(path, fmt, levels):
    """DXT levels (w, h, bytes), largest first; fmt 0 for 32-bit RGBA bytes (written as A8R8G8B8)"""
    w, h = levels[0][0], levels[0][1]
    dxt = fmt in FORMATS
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | (0x80000 if dxt else 0x8) | (0x20000 if len(levels) > 1 else 0)
    pitch = len(levels[0][2]) if dxt else w * 4
    header = struct.pack('<4sIIIIIII44x', b'DDS ', 124, flags, h, w, pitch, 0, len(levels))
    if dxt:
        pixel = struct.pack('<II4s20x', 32, 0x4, FORMATS[fmt][0].encode())
    else:
        pixel = struct.pack('<IIIIIIII', 32, 0x41, 0, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
    caps = struct.pack('<IIII4x', 0x1000 | (0x8 | 0x400000 if len(levels) > 1 else 0), 0, 0, 0)
    with open(path, 'wb') as f:
        f.write(header + pixel + caps)
        for _, _, data in levels:
            if not dxt:
                b = bytearray(data)
                b[0::4], b[2::4] = data[2::4], data[0::4]
                data = bytes(b)
            f.write(data)


def keep_game_levels(path, t, alpha_only=False):
    """TextureLogic::KeepGameLevels: a DDS with the game's own levels, byte for byte, in place of its levels of those
    sizes (t is parse()'s), or KeepGameAlpha: a DXT3 or DXT5 block's alpha alone, its first 8 bytes"""
    with open(path, 'rb') as f:
        data = bytearray(f.read())
    fmt, w, h, count = read_dds_header(path)
    if fmt != t['format']:
        raise ValueError(f'{path}: not the game\'s format')
    at, places = 128, {}
    for _ in range(count):
        size = level_bytes(fmt, w, h)
        places[(w, h)] = (at, size)
        at += size
        w, h = max(1, w // 2), max(1, h // 2)
    for lw, lh, level in t['levels']:
        if (lw, lh) in places:
            place, size = places[(lw, lh)]
            if alpha_only:
                for at in range(0, size, 16):
                    data[place + at:place + at + 8] = level[at:at + 8]
            else:
                data[place:place + size] = level
    with open(path, 'wb') as f:
        f.write(data)


def read_dds_header(path):
    """(format, width, height, levels): format 11/12/13 or 0 for A8R8G8B8; None for other DDS"""
    with open(path, 'rb') as f:
        head = f.read(128)
    if len(head) < 128 or head[:4] != b'DDS ':
        return None
    h, w = struct.unpack_from('<II', head, 12)
    count = max(1, struct.unpack_from('<I', head, 28)[0])
    if struct.unpack_from('<I', head, 80)[0] & 0x4:
        fourcc = head[84:88].decode('latin1')
        fmt = {v[0]: k for k, v in FORMATS.items()}.get(fourcc)
        return None if fmt is None else (fmt, w, h, count)
    return (0, w, h, count) if struct.unpack_from('<I', head, 88)[0] == 32 else None
