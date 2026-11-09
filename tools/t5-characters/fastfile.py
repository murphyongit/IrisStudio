"""Reads Black Ops (T5) PC fastfiles offline: the asset list, the script rawfiles and the models' bone counts.

A PC zone is "IWffu100", the version (0x1D9) and one zlib stream, which inflates to the XFile header (36 bytes),
the XAssetList (script strings, then {type, header} pairs) and the assets themselves, each written as its struct
with -1 for every pointer whose data follows. No signature, no encryption.

    python fastfile.py <zone.ff>                       asset counts by type
    python fastfile.py <zone.ff> scripts [prefix ...]  the text of rawfiles (default character/ aitype/ xmodelalias/)
    python fastfile.py <zone.ff> models [filter]       the models with their bones and surfaces
"""
import re
import struct
import sys
import zlib
from collections import Counter

MAGIC = b"IWffu100"
VERSION = 0x1D9

# the asset types, by their number in the asset list (as t5/src/Structures.hpp has them)
TYPES = ["xmodelpieces", "physpreset", "physconstraints", "destructibledef", "xanim", "xmodel", "material",
         "techset", "image", "sound", "sound_patch", "clipmap", "clipmap_pvs", "comworld", "gameworld_sp",
         "gameworld_mp", "map_ents", "gfxworld", "lightdef", "ui_map", "font", "menulist", "menu", "localize",
         "weapon", "weapondef", "weaponvariant", "snddriverglobals", "fx", "impactfx", "aitype", "mptype",
         "mpbody", "mphead", "character", "xmodelalias", "rawfile", "stringtable", "packindex", "xglobals",
         "ddl", "glasses", "emblemset"]

XFILE_SIZE = 36  # {size, externalSize, blockSize[7]}
FOLLOWS = 0xFFFFFFFF  # a pointer whose data is written right after


def inflate(path):
    """The zone's inflated stream."""
    with open(path, "rb") as f:
        head = f.read(12)
        if head[:8] != MAGIC:
            raise ValueError(f"{path} is not a PC fastfile ({head[:8]!r})")
        version = struct.unpack_from("<I", head, 8)[0]
        if version != VERSION:
            raise ValueError(f"{path} has version {version:#x}, not {VERSION:#x}")
        return zlib.decompress(f.read())


def asset_counts(data):
    """How many assets of each type the zone lists."""
    string_count, _, asset_count, _ = struct.unpack_from("<IIII", data, XFILE_SIZE)
    pos = XFILE_SIZE + 16
    pointers = struct.unpack_from(f"<{string_count}I", data, pos)
    pos += 4 * string_count
    for pointer in pointers:
        if pointer == FOLLOWS:
            pos = data.index(b"\0", pos) + 1
    return Counter(struct.unpack_from("<I", data, pos + 8 * i)[0] for i in range(asset_count))


# a rawfile is {name, length, buffer} with both pointers following: the name, then {u32 inflated size, u32 deflated
# size, zlib data}; length is the deflated size + 8
RAWFILE_RE = re.compile(rb"\xff\xff\xff\xff(....)\xff\xff\xff\xff([a-z0-9_]+(?:/[a-z0-9_\-.]+)+)\0", re.S)


def rawfiles(data, prefixes=("character/", "aitype/", "xmodelalias/"), tolerant=False):
    """The text of the zone's rawfiles whose names start with one of the prefixes, by name. Tolerant skips what only
    looks like a rawfile (some materials do, which a prefix of "" finds) instead of raising."""
    texts = {}
    for m in RAWFILE_RE.finditer(data):
        name = m.group(2).decode()
        if not name.startswith(tuple(prefixes)):
            continue
        length = struct.unpack("<I", m.group(1))[0]
        pos = m.end()
        size, deflated = struct.unpack_from("<II", data, pos)
        try:
            if deflated + 8 != length:
                raise ValueError(f"rawfile {name}: length {length} doesn't match {deflated} + 8")
            text = zlib.decompress(data[pos + 8:pos + 8 + deflated])
            if len(text) != size:
                raise ValueError(f"rawfile {name}: {len(text)} bytes inflated, {size} expected")
        except (ValueError, zlib.error):
            if tolerant:
                continue
            raise
        texts[name] = text.decode("latin1")
    return texts


# An XModel is its 0xFC byte struct (the name pointer first), then its name: bones at +4, root bones at +5, surfaces
# at +6. Models written before others can share their skeleton (the _char twins do), so the arrays after the name
# aren't read here.
XMODEL_SIZE = 0xFC
XMODEL_RE = re.compile(rb"\xff\xff\xff\xff(?=.{248}([a-z][a-z0-9_]{2,80})\0)", re.S)


def models(data, part=""):
    """The zone's models: {name: (bones, root bones, surfaces)}."""
    found = {}
    for m in XMODEL_RE.finditer(data):
        name = m.group(1).decode()
        if part and part not in name:
            continue
        start = m.start()
        bones, roots, surfaces = data[start + 4], data[start + 5], data[start + 6]
        if not bones or not roots or not surfaces or roots > bones or roots > 8:
            continue
        # the surfaces and materials are always written with the model
        surfaces_pointer, materials_pointer = struct.unpack_from("<II", data, start + 8 + 4 * 6)
        if surfaces_pointer != FOLLOWS or materials_pointer != FOLLOWS:
            continue
        found.setdefault(name, (bones, roots, surfaces))
    return found


def main():
    data = inflate(sys.argv[1])
    what = sys.argv[2] if len(sys.argv) > 2 else "counts"
    if what == "scripts":
        for name, text in sorted(rawfiles(data, sys.argv[3:] or ("character/", "aitype/", "xmodelalias/")).items()):
            print(f"--- {name}")
            print(text)
    elif what == "models":
        for name, (bones, roots, surfaces) in sorted(models(data, sys.argv[3] if len(sys.argv) > 3 else "").items()):
            print(f"{name:56s} bones {bones:3d} ({roots} root) surfaces {surfaces}")
    else:
        print(f"{sys.argv[1]}: {len(data) / 2 ** 20:.1f} MB inflated")
        for number, count in sorted(asset_counts(data).items()):
            print(f"  {number:2d} {TYPES[number] if number < len(TYPES) else '?':16s} {count}")


if __name__ == "__main__":
    main()
