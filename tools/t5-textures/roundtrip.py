"""python roundtrip.py <zone.ff> [--game <folder>]: every DXT image of a zone parsed and written back (iwi.py, the same
layout as TextureLogic::WriteIwi); prints how many came back byte for byte."""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import iwi  # noqa: E402
import zone  # noqa: E402

parser = argparse.ArgumentParser()
parser.add_argument('zone')
parser.add_argument('--game', required=True)
args = parser.parse_args()
idx = iwi.index(iwi.search_folders(args.game))
same = other = 0
for name in zone.images(args.zone, set(idx)):
    path, entry, size, crc = idx[name]
    data = iwi.read(path, entry)
    t = iwi.parse(data)
    if t['format'] not in iwi.FORMATS or t['flags'] & (iwi.CUBE | iwi.VOLUME):
        other += 1
        continue
    if iwi.write_iwi(t['format'], t['flags'], t['gamma'], t['levels'], t['depth']) == data:
        same += 1
    else:
        print('differs', name)
print(f'{same} written back byte for byte, {other} not flat DXT')
