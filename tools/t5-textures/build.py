"""Builds a texture pack from the command line, the way Iris Studio does, to check its
output against and to make small packs for the harness.

    python build.py --out <textures folder> --zones mp_nuked [common_mp ...] [--scale 4] [--only <image> ...]
                    [--plain] [--uncompressed] [--esrgan <realesrgan-ncnn-vulkan.exe> --models <folder>]
                    [--texconv <texconv.exe>] [--game <folder>] [--storage <Plutonium storage\\t5>]

Each colour, normal and spec map of the zones (zone.py's kinds; each once) is read from the iwds and decoded, then:
colour and normal maps (normal maps rebuilt as RGB normals) wrap-padded 32 texels and run through the model at 4x
(4xNomos8kSC, AC1), or Lanczos with --plain; spec maps Lanczos; normal maps renormalized and packed back (x in alpha,
y in green); alpha resized plainly; halved for 2x or the 4,096 cap; then texconv to the game's DXT format with mips,
alpha apart (-sepalpha), a spec map keeping the game's own levels from its size down; or 32-bit DDS with
box-filtered mips with --uncompressed (no texconv needed). Written to <out>\\v1\\<image>.dds with <out>\\v1\\index.json,
merged with what is there. --texconv auto takes the one Iris Studio downloaded; --old-mips builds the old way,
for the scenario that shows its check sees the difference.
"""
import argparse
import concurrent.futures
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check  # noqa: E402
import dxt  # noqa: E402
import iwi  # noqa: E402
import zone  # noqa: E402

MAX_SIDE = 4096
PAD = 32
CREDIT = '4xNomos8kSC by Helaman (CC-BY 4.0)'


def to_normal(rgba):
    x = rgba[..., 3].astype(np.float32) / 255 * 2 - 1
    y = rgba[..., 1].astype(np.float32) / 255 * 2 - 1
    z = np.sqrt(np.clip(1 - x * x - y * y, 0, 1))
    return np.clip(np.round(np.stack([x, y, z], -1) * 127.5 + 127.5), 0, 255).astype(np.uint8)


def from_normal(rgb):
    n = rgb[..., :3].astype(np.float32) / 127.5 - 1
    n /= np.maximum(np.linalg.norm(n, axis=-1, keepdims=True), 1e-6)
    x = np.clip(np.round(n[..., 0] * 127.5 + 127.5), 0, 255).astype(np.uint8)
    y = np.clip(np.round(n[..., 1] * 127.5 + 127.5), 0, 255).astype(np.uint8)
    return np.stack([y, y, y, x], -1)


def lanczos(a, w, h):
    # (colour and alpha apart: Pillow's resize of RGBA goes through premultiplied alpha, losing the colour under low
    # alpha)
    if a.shape[-1] == 4:
        return np.dstack([lanczos(a[..., :3], w, h), lanczos(a[..., 3], w, h)])
    return np.asarray(Image.fromarray(np.ascontiguousarray(a)).resize((w, h), Image.LANCZOS))


def halve(a, normal):
    if normal:
        return from_normal(halve(to_normal(a), False))
    h, w = a.shape[:2]
    a = a[:max(2, h - h % 2), :max(2, w - w % 2)].astype(np.uint16) if h > 1 and w > 1 else a.astype(np.uint16)
    if a.shape[0] < 2 or a.shape[1] < 2:
        return a.astype(np.uint8)
    return ((a[0::2, 0::2] + a[1::2, 0::2] + a[0::2, 1::2] + a[1::2, 1::2] + 2) // 4).astype(np.uint8)


def run_model(jobs, esrgan, models, work):
    """4x of each (name, rgb array) through the model in one run, wrap-padded: a function giving each one's 4x,
    cropped back, read from the model's output when asked for (a whole map's 4x doesn't fit in memory)"""
    src, dst = os.path.join(work, 'in'), os.path.join(work, 'out')
    for d in (src, dst):
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
    pads = {}
    for i, (name, rgb) in enumerate(jobs):
        h, w = rgb.shape[:2]
        p = min(PAD, w // 2, h // 2)
        pads[name] = (i, p, w, h)
        Image.fromarray(np.pad(rgb, ((p, p), (p, p), (0, 0)), mode='wrap')).save(os.path.join(src, f'{i:04d}.png'))
    subprocess.run([esrgan, '-i', src, '-o', dst, '-n', '4xNomos8kSC', '-s', '4', '-m', models, '-t', '256', '-f',
                    'png'], check=True, capture_output=True)

    def get(name):
        i, p, w, h = pads[name]
        a = np.asarray(Image.open(os.path.join(dst, f'{i:04d}.png')).convert('RGB'))
        return a[p * 4:p * 4 + h * 4, p * 4:p * 4 + w * 4]
    return get, set(pads)


def write_texture(path, rgba, fmt, normal, game, args, work):
    """the pack's file: texconv to the game's format with mips, a spec map's (game) from its size down the game's, or
    32-bit with box mips"""
    if args.uncompressed or not args.texconv:
        levels, a = [], rgba
        while True:
            levels.append((a.shape[1], a.shape[0], a.tobytes()))
            if a.shape[0] == 1 and a.shape[1] == 1:
                break
            a = halve(a, normal) if a.shape[0] > 1 and a.shape[1] > 1 else np.ascontiguousarray(a[::2, ::2])
        iwi.write_dds(path, 0, levels)
        return
    png = os.path.join(work, 'texconv.png')
    Image.fromarray(rgba).save(png)
    subprocess.run([args.texconv, '-nologo', '-y', '-dx9', *([] if args.old_mips else ['-sepalpha']), '-m', '0', '-f',
                    {11: 'BC1_UNORM', 12: 'BC2_UNORM', 13: 'BC3_UNORM'}[fmt], '-o', work, png], check=True,
                   capture_output=True)
    if game and not args.old_mips:
        iwi.keep_game_levels(os.path.join(work, 'texconv.dds'), game[0], game[1])
    os.replace(os.path.join(work, 'texconv.dds'), path)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', required=True)
    parser.add_argument('--zones', nargs='+', required=True)
    parser.add_argument('--scale', type=int, default=4, choices=(2, 4))
    parser.add_argument('--only', nargs='*')
    parser.add_argument('--plain', action='store_true')
    parser.add_argument('--uncompressed', action='store_true')
    parser.add_argument('--esrgan')
    parser.add_argument('--models')
    parser.add_argument('--texconv')
    parser.add_argument('--game', required=True)
    parser.add_argument('--storage')
    # for tests: colour maps' red, green and blue scaled, so the harness can tell the pack's texture from the game's
    parser.add_argument('--tint', type=lambda v: [float(x) for x in v.split(',')])
    parser.add_argument('--old-mips', action='store_true')
    args = parser.parse_args()
    if args.texconv == 'auto':
        args.texconv = os.path.expandvars(r'%LOCALAPPDATA%\IrisStudio\tools\textures\texconv.exe')
    if not args.plain and not args.esrgan:
        parser.error('--esrgan and --models, or --plain')

    idx = iwi.index(iwi.search_folders(args.game, args.storage))
    names = set(idx)
    folder = os.path.join(args.out, 'v1')
    os.makedirs(folder, exist_ok=True)
    index_path = os.path.join(folder, 'index.json')
    index = json.load(open(index_path)) if os.path.exists(index_path) else {'version': 1, 'parts': {}, 'images': {}}
    index['model'] = 'Lanczos' if args.plain else CREDIT
    only = {n.lower() for n in args.only} if args.only else None

    wanted = {}
    for z in args.zones:
        path = os.path.join(args.game, 'zone', 'Common', z + '.ff')
        for name in zone.images(path, names):
            if only is not None and name not in only:
                continue
            wanted.setdefault(name, []).append(z)
        index['parts'][z] = {'scale': args.scale}
    work = tempfile.mkdtemp(prefix='t5tex')
    started, done = time.time(), 0
    model_jobs, items = [], []
    for name, parts in sorted(wanted.items()):
        p, entry, size, crc = idx[name]
        t = iwi.parse(iwi.read(p, entry))
        if not zone.upscaled(name, t):
            continue
        k = zone.kind(name, t['format'])
        w, h, data = t['levels'][0]
        rgba = dxt.decode(t['format'], w, h, data)
        items.append((name, parts, t, k, rgba, size, crc))
        if k in ('colour', 'normal') and not args.plain:
            model_jobs.append((name, to_normal(rgba)[..., :3] if k == 'normal' else rgba[..., :3]))
    upscaled, modelled = run_model(model_jobs, args.esrgan, args.models, work) if model_jobs else (None, set())
    model_jobs = None

    def finish(job):
        name, parts, scale, size, crc, out, up, fmt, normal, game = job
        own = os.path.join(work, 'c' + str(abs(hash(name))))
        os.makedirs(own, exist_ok=True)
        write_texture(out, up, fmt, normal, game, args, own)
        shutil.rmtree(own, ignore_errors=True)
        return name, parts, scale, size, crc, out
    pool = concurrent.futures.ThreadPoolExecutor(12)
    pending, finished = [], []
    for name, parts, t, k, rgba, size, crc in items:
        w, h, fmt = t['width'], t['height'], t['format']
        normal = k == 'normal'
        given = upscaled(name) if name in modelled else None
        if given is not None:
            # what the model gave back kept only when it is the same picture (a run of it can give back black)
            shown = to_normal(rgba) if normal else rgba[..., :3]
            colour, _, _ = check.compare(np.dstack([shown, np.full(shown.shape[:2], 255, np.uint8)]),
                                         np.dstack([given, np.full(given.shape[:2], 255, np.uint8)]))
            if colour > check.WRONG_LIMIT:
                print(f'{name}: the model gave back something else, resized plainly')
                given = None
        if given is not None:
            up = from_normal(given) if normal else np.concatenate([given, lanczos(rgba, w * 4, h * 4)[..., 3:]], -1)
        else:
            up = from_normal(lanczos(to_normal(rgba), w * 4, h * 4)) if normal else lanczos(rgba, w * 4, h * 4)
        scale = 4
        while scale > args.scale or w * scale > MAX_SIDE or h * scale > MAX_SIDE:
            if scale == 1:
                break
            up = halve(up, normal)
            scale //= 2
        if args.tint and k == 'colour':
            up = up.copy()
            up[..., :3] = np.clip(up[..., :3] * np.array(args.tint, np.float32), 0, 255).astype(np.uint8)
        if not normal and fmt == 11 and rgba[..., 3].min() == 255:
            up = up.copy()
            up[..., 3] = 255
        out = os.path.join(folder, name + '.dds')
        pending.append(pool.submit(finish, (name, parts, scale, size, crc, out, np.ascontiguousarray(up), fmt, normal,
                                            (t, False) if k == 'spec' else (t, True) if k == 'colour' and fmt != 11 else
                                            None)))
        # a few at a time: each holds its 4x
        while len(pending) > 24:
            finished.append(pending.pop(0).result())
    finished += [job.result() for job in pending]
    pool.shutdown()
    for name, parts, scale, size, crc, out in finished:
        head = iwi.read_dds_header(out)
        index['images'][name] = {'scale': scale, 'format': head[0], 'width': head[1], 'height': head[2],
                                 'bytes': os.path.getsize(out), 'source': {'size': size, 'crc': crc},
                                 'parts': sorted(set(index['images'].get(name, {}).get('parts', [])) | set(parts))}
        if not args.old_mips:
            index['images'][name]['build'] = 2
        done += 1
    for z, part in index['parts'].items():
        mine = [i for i in index['images'].values() if z in i.get('parts', [])]
        part.update(images=len(mine), bytes=sum(i['bytes'] for i in mine))
    json.dump(index, open(index_path, 'w'), indent=1)
    shutil.rmtree(work, ignore_errors=True)
    print(f'{done} textures in {time.time() - started:.0f} s into {folder}')


if __name__ == '__main__':
    main()
