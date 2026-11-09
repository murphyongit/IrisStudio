"""Checks a texture pack against the game's own images: each image's level at the
game's size (a spec map's small level) beside the game's, as Iris Studio's check does (TextureBuild::Compare, IsWrong):
a good image only differs in detail. Reads only; it writes nothing near the pack.

    python -I check.py <pack's v1 folder> [--game <folder>] [--storage <Plutonium storage\\t5>] [--list] [--processes 10]

It prints the images wrong by kind and format, with --list each one with its numbers, and exits 1 when any is wrong.
"""
import argparse
import collections
import json
import multiprocessing
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dxt  # noqa: E402
import iwi  # noqa: E402
import zone  # noqa: E402

WRONG_LIMIT = 24.0
WRONG_SPEC_LIMIT = 6.0
CHECK_SIDE = 64
LOW_ALPHA = 64


def cells(a, across, down):
    """the mean of each channel in each cell of a grid, as TextureBuild's GetCells"""
    h, w = a.shape[:2]
    ys, xs = np.arange(h) * down // h, np.arange(w) * across // w
    cell = ys[:, None] * across + xs[None, :]
    counts = np.bincount(cell.ravel(), minlength=across * down).astype(np.float64)
    return np.stack([np.bincount(cell.ravel(), a[..., c].ravel().astype(np.float64), across * down) / np.maximum(counts, 1)
                     for c in range(4)], -1)


def compare(game, pack):
    """colour, alpha, under alpha: TextureBuild::Compare"""
    across, down = min(8, game.shape[1], pack.shape[1]), min(8, game.shape[0], pack.shape[0])
    a, b = cells(game, across, down), cells(pack, across, down)
    colour = float(np.abs(a - b)[:, :3].max())
    alpha = float(np.abs(a - b)[:, 3].max())
    under = 0.0
    if game.shape == pack.shape:
        low = game[..., 3] < LOW_ALPHA
        if low.sum() * 100 >= low.size:
            under = float(np.abs(game[low][:, :3].astype(np.float64).mean(0) - pack[low][:, :3].astype(np.float64).mean(0)).max())
    return colour, alpha, under


def is_wrong(kind, colour, alpha, under):
    """TextureBuild::IsWrong"""
    if kind == 'spec':
        return max(colour, alpha) > WRONG_SPEC_LIMIT
    return max(colour, alpha) > WRONG_LIMIT or (kind != 'normal' and under > WRONG_LIMIT)


def check_level(kind, t):
    """TextureLogic::GetCheckLevel"""
    level, w, h = 0, t['width'], t['height']
    while kind == 'spec' and not t['flags'] & iwi.NO_MIPS and max(w, h) > CHECK_SIDE:
        w, h = max(1, w // 2), max(1, h // 2)
        level += 1
    return level


def one(job):
    name, folder, where = job
    path, entry = where[0], where[1]
    t = iwi.parse(iwi.read(path, entry))
    if t['format'] not in iwi.FORMATS:
        return name, None, 'not DXT', 0, 0, 0
    kind = zone.kind(name, t['format'])
    w, h, data = t['levels'][check_level(kind, t)]
    file = os.path.join(folder, name + '.dds')
    pack = dxt.read_dds_level(file, w, h)
    if pack is None:
        return name, t['format'], kind, -1, -1, -1
    colour, alpha, under = compare(dxt.decode(t['format'], w, h, data), pack)
    # a colour map's alpha from the game's size down is the game's: at a small level, as a spec map's levels
    if kind == 'colour' and t['format'] != 11 and not t['flags'] & iwi.NO_MIPS:
        w, h, data = t['levels'][check_level('spec', t)]
        small = dxt.read_dds_level(file, w, h)
        if small is not None and compare(dxt.decode(t['format'], w, h, data), small)[1] > WRONG_SPEC_LIMIT:
            alpha = max(alpha, WRONG_LIMIT + 1)
    return name, t['format'], kind, colour, alpha, under


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('folder')
    parser.add_argument('--game', required=True)
    parser.add_argument('--storage')
    parser.add_argument('--list', action='store_true')
    parser.add_argument('--processes', type=int, default=10)
    args = parser.parse_args()
    index = json.load(open(os.path.join(args.folder, 'index.json')))
    found = iwi.index(iwi.search_folders(args.game, args.storage))
    jobs = [(name, args.folder, found[name]) for name in index['images'] if name in found]
    wrong, right, unknown = collections.Counter(), collections.Counter(), 0
    lines = []
    with multiprocessing.Pool(args.processes) as pool:
        for name, fmt, kind, colour, alpha, under in pool.imap_unordered(one, jobs, 20):
            if colour < 0 or fmt is None:
                unknown += 1
            elif is_wrong(kind, colour, alpha, under):
                wrong[(kind, fmt)] += 1
                lines.append(f'{name}: {kind} DXT{ {11: 1, 12: 3, 13: 5}[fmt] }, colour {colour:.1f}, alpha {alpha:.1f}, '
                             f'under alpha {under:.1f}')
            else:
                right[(kind, fmt)] += 1
    if args.list:
        for line in sorted(lines):
            print(line)
    print(f'{len(jobs)} images: {sum(right.values())} right, {sum(wrong.values())} wrong, {unknown} not compared')
    for key in sorted(set(wrong) | set(right), key=str):
        print(f'  {key[0]} DXT{ {11: 1, 12: 3, 13: 5}[key[1]] }: {right[key]} right, {wrong[key]} wrong')
    sys.exit(1 if wrong else 0)


if __name__ == '__main__':
    main()
