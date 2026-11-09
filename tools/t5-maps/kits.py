"""Builds the campaign maps' kits with OpenAssetTools' Linker: one zone a teamset
(t5m_kit_<teamset>) of what multiplayer maps carry beside their world, the players' body, head and dog scripts, the
teamset's player bodies, heads and arms, the RC-XD, the bomb, the HQ crate and the RC-XD's effects, linked out of the
first multiplayer map of that teamset the game has, as core/resources/campaign-kits.json lists them. T5iris loads a
campaign level's kit with it (t5/src/CampaignMaps.cpp). The launcher builds the same zones (SetupLogic::ReadKitZones).

    python -I tools/t5-maps/kits.py --game <game folder> --linker <Linker.exe> --work <folder> --out <folder>
                                    [--kits campaign-kits.json]
"""
import argparse
import json
import os
import shutil
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
# multiplayer's zones a map's references go to, loaded before it (each after its language twin)
MP_BASE = ["code_post_gfx_mp", "patch_mp", "common_mp"]


def find_zone(game, name):
    """the language twin (zone/<language>/en_<name>.ff), then zone/Common/<name>.ff, those there are"""
    found = []
    folder = os.path.join(game, "zone")
    for language in sorted(os.listdir(folder)):
        if language.lower() == "common" or not os.path.isdir(os.path.join(folder, language)):
            continue
        for file in sorted(os.listdir(os.path.join(folder, language))):
            if file.lower().endswith("_" + name + ".ff") and len(file) == len(name) + 6:
                found.append(os.path.join(folder, language, file))
                break
        if found:
            break
    common = os.path.join(folder, "Common", name + ".ff")
    if os.path.isfile(common):
        found.append(common)
    return found


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--game", required=True)
    parser.add_argument("--linker", required=True)
    parser.add_argument("--work", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--kits", default=os.path.join(REPO, "core", "resources", "campaign-kits.json"))
    args = parser.parse_args()
    kits = json.load(open(args.kits, encoding="utf-8"))
    if kits.get("version") != 1:
        raise SystemExit(f"{args.kits} isn't of version 1")
    os.makedirs(os.path.join(args.work, "zone_source"), exist_ok=True)
    os.makedirs(os.path.join(args.work, "zone_out"), exist_ok=True)
    os.makedirs(args.out, exist_ok=True)

    failed = 0
    for kit in kits["kits"]:
        name = kit["zone"]
        source = next((s for s in kit["sources"] if os.path.isfile(os.path.join(args.game, "zone", "Common",
                                                                                s + ".ff"))), None)
        if not source:
            print(f"{name}: the game has none of {', '.join(kit['sources'])}")
            failed += 1
            continue
        models = [n for t, n in kit["assets"] if t == "xmodel"]
        others = [(t, n) for t, n in kit["assets"] if t != "xmodel"]
        with open(os.path.join(args.work, "zone_source", name + ".zone"), "w", newline="\n") as f:
            f.write(f">game,T5\n>name,{name}\n\n" + "".join(f"xmodel,{m}\n" for m in models) +
                    "".join(f"{t},{n}\n" for t, n in others))
        command = [args.linker, "--no-color", "--base-folder", args.work, "--output-folder",
                   os.path.join(args.work, "zone_out")]
        for zone in MP_BASE + [source]:
            for path in find_zone(args.game, zone):
                command += ["--load", path]
        command.append(name)
        result = subprocess.run(command, capture_output=True, text=True, errors="replace",
                                cwd=os.path.dirname(args.linker))
        with open(os.path.join(args.work, name + ".log"), "w", encoding="utf-8") as f:
            f.write(result.stdout + result.stderr)
        built = os.path.join(args.work, "zone_out", name + ".ff")
        if not os.path.isfile(built):
            print(f"{name}: not built (the Linker's log is in {args.work})")
            failed += 1
            continue
        shutil.copyfile(built, os.path.join(args.out, name + ".ff"))
        print(f"{name}: {os.path.getsize(built) / 1e6:.1f} MB, out of {source}")
    sys.exit(1 if failed else 0)


main()
