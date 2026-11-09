"""Copies the pictures of the game's Change Map menu for campaign levels and Zombies maps
out of the game's single player zones, with OpenAssetTools' Unlinker: the campaign's mission select pictures
(zone/Common/frontend.ff) and the Zombies map select's (zone/Common/patch.ff), as core/resources/campaign-kits.json
lists them, written as <image>.dds into the out folder (the mod's pictures folder, beside its zones). T5iris shows
them as the menu's pictures (t5/src/MapMenu.cpp). The launcher's setup does the same (SetupLogic::ReadMenuPictures).

    python -I tools/t5-maps/pictures.py --game <game folder> --unlinker <Unlinker.exe> --work <folder> --out <folder>
                                        [--kits campaign-kits.json]
"""
import argparse
import json
import os
import shutil
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--game", required=True)
    parser.add_argument("--unlinker", required=True)
    parser.add_argument("--work", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--kits", default=os.path.join(REPO, "core", "resources", "campaign-kits.json"))
    args = parser.parse_args()

    pictures = json.load(open(args.kits, encoding="utf-8"))["pictures"]
    os.makedirs(args.out, exist_ok=True)
    written, missing = 0, []
    for zone in sorted({p["zone"] for p in pictures}):
        path = os.path.join(args.game, "zone", "Common", zone + ".ff")
        wanted = [p["image"] for p in pictures if p["zone"] == zone]
        if not os.path.isfile(path):
            missing += wanted
            continue
        folder = os.path.join(args.work, zone)
        subprocess.run([args.unlinker, "--include-assets", "image", "--image-format", "DDS", "-o", folder, path],
                       capture_output=True)
        for image in wanted:
            dumped = os.path.join(folder, "images", image + ".dds")
            if os.path.isfile(dumped):
                shutil.copyfile(dumped, os.path.join(args.out, image + ".dds"))
                written += 1
            else:
                missing.append(image)
    shutil.rmtree(args.work, ignore_errors=True)
    print(f"{written} of {len(pictures)} pictures written to {args.out}" +
          (f"; not in the game's zones: {', '.join(missing)}" if missing else ""))
    sys.exit(1 if missing else 0)


main()
