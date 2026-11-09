"""A zone's images and their kinds, without OpenAssetTools.

An image of a PC zone is written with its name following ({..., name: -1}, then 4 bytes, the name and a 0): every such
record whose name the iwds have is one of the zone's images. Of the textures a pack upscales, that finds exactly the
ones OpenAssetTools' `Unlinker --list` lists (Nuketown 931 of 931, common_mp 561 of 561, none more); it misses only HUD
images (2 on Nuketown, 385 in common_mp), which aren't upscaled. Iris Studio reads zones the same way
(launcher/src/TexturePack.cpp).

    python zone.py <zone.ff> [--game <folder>]     the zone's images with their kind, format and size
"""
import argparse
import os
import re
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import iwi  # noqa: E402

RECORD = re.compile(rb'\xff\xff\xff\xff[\s\S]{4}([\x21-\x7e]+)\x00')


def inflate(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:8] != b'IWffu100':
        raise ValueError(f'{path} is not a PC fastfile')
    return zlib.decompress(data[12:])


def images(zone_path, names):
    """the zone's image names that names (the iwds' image names) has"""
    data = inflate(zone_path)
    return sorted({m.group(1).decode('latin1').lower() for m in RECORD.finditer(data)} & names)


def ends_with_any(n, ends):
    n = n[:-1] if n and n[-1].isdigit() else n
    return any(n.endswith(e) for e in ends)


def has_word_starting(n, starts):
    return any((i == 0 or n[i - 1] in '_/') and n[i:].startswith(s) for i in range(len(n)) for s in starts)


def has_word(n, words):
    for w in n.split('_'):
        if (w[:-1] if w and w[-1].isdigit() else w) in words:
            return True
    return False


def kind(name, fmt=13):
    """TextureLogic::GetKind: colour, normal, spec, ui, effect, other"""
    n = name.lower()
    if n.startswith(('$', 'hud', 'menu', 'ui_', 'compass', 'loadscreen', 'cac_', 'em_', 'rank_', 'killicon', 'reticle',
                     'mtl_hud', 'mp_hud')) or has_word_starting(n, (
                         'hud', 'menu', 'ui', 'compass', 'loadscreen', 'cac', 'emblem', 'rank', 'killicon', 'minimap',
                         'font', 'gamefont', 'scoreboard', 'headicon', 'playlist', 'lui', 'flag_', 'faction', 'logo',
                         'cursor', 'reticle', 'overlay', 'splash', 'white', 'black', 'gradient', 'button')):
        return 'ui'
    if n.startswith(('fx', 'sky')) or has_word(n, {
            'fx', 'fxt', 'smoke', 'fire', 'flame', 'flames', 'spark', 'sparks', 'explosion', 'muzzle', 'muzzleflash',
            'blood', 'tracer', 'glow', 'flare', 'lens', 'lensflare', 'splash', 'ripple', 'ripples', 'steam', 'cloud',
            'clouds', 'sky', 'skybox', 'lightning', 'beam', 'shockwave', 'distort', 'distortion'}):
        return 'effect'
    if ends_with_any(n, ('_n', '_nml', '_normal', '_nrm')) or '_n_' in n:
        return 'normal' if fmt == 13 else 'spec'
    if ends_with_any(n, ('_s', '_g', '_spc', '_spec', '_gloss', '_sc', '_q', '_ao', '_m', '_o', '_mask', '_cos')) or \
            n.startswith(('~~', '~$')):
        return 'spec'
    if ends_with_any(n, ('_c', '_col', '_color', '_d', '_diffuse', '_c_d')) or n.startswith('~-g'):
        return 'colour'
    return 'other'


def upscaled(name, t):
    """TextureLogic::IsUpscaled"""
    return (t['format'] in iwi.FORMATS and t['depth'] <= 1 and not t['flags'] & (iwi.CUBE | iwi.VOLUME) and
            t['width'] >= 4 and t['height'] >= 4 and kind(name, t['format']) in ('colour', 'normal', 'spec'))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('zone')
    parser.add_argument('--game', required=True)
    args = parser.parse_args()
    idx = iwi.index(iwi.search_folders(args.game))
    for name in images(args.zone, set(idx)):
        path, entry, size, crc = idx[name]
        t = iwi.parse_header(iwi.read(path, entry)[:48])
        print(name, kind(name, t['format']), t['format'], t['width'], t['height'], 'up' if upscaled(name, t) else '-')
