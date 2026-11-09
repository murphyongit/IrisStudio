"""Builds character zones with OpenAssetTools' Linker: one small fastfile per character, holding its models and what
they draw with (materials, technique sets, image headers), copied out of the game's own zones. The mod loads them
when an actor wears the character (see t5/src/ContentPacks.cpp).

A level zone references some of its materials by name from its language twin (zone/<language>/en_<level>.ff) and
from patch.ff, which the game always loads with the level, so the Linker loads those (and the mode's common zones)
before the level: the character zone then carries copies of everything its models use.

    python zones.py --game <game folder> --linker <Linker.exe> --work <folder> --out <folder>
                    [--catalog characters.json] [--only <character id> ...]
                    [--define <zone name>=<level>:<model>+<model>...]

Without --define every character of the catalog (or those --only names) gets a zone; with it only the zones defined
there are built. Each level's zones are linked in one Linker run (the level zone is read once). The zones are
written to --out (the mod's storage/t5/iris/zones); --work keeps the definitions and the Linker's logs.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from collections import defaultdict

# the zones a level needs loaded before it, as the game loads them: single player levels go with patch and common,
# Zombies maps with common_zombie and their own patch zone
SP_BASE = ["patch", "common"]
ZOMBIES_BASE = ["patch", "common_zombie", "common_zombie_patch"]


def zone_name(character_id):
    """The character zone of a catalog id ("khe_sanh/c_usa_jungmar_assault" -> "t5c_khe_sanh_usa_jungmar_assault")."""
    level, script = character_id.split("/")
    if script.startswith("c_"):
        script = script[2:]
    return f"t5c_{level}_{script}"


def character_models(character):
    """Every model a catalog character can be made of: its parts, the arrays they are picked from, its wounded bodies
    and the limbs its wounds throw off."""
    models = [character["body"], character.get("head"), character.get("hat"), character.get("gear")]
    for variants in character.get("variants", {}).values():
        models += variants
    damage = character.get("damage", {})
    models += damage.get("torso", []) + damage.get("legs", []) + [piece["model"] for piece in damage.get("pieces", [])]
    unique = []
    for model in models:
        if model and model not in unique:
            unique.append(model)
    return unique


def find_zone(game, name, localized=False):
    """A zone of the game: zone/Common/<name>.ff, or with localized its twin in a language folder (en_<name>.ff)."""
    zone_folder = os.path.join(game, "zone")
    if not localized:
        path = os.path.join(zone_folder, "Common", name + ".ff")
        return path if os.path.isfile(path) else None
    for language in sorted(os.listdir(zone_folder)):
        folder = os.path.join(zone_folder, language)
        if language.lower() == "common" or not os.path.isdir(folder):
            continue
        for file in sorted(os.listdir(folder)):
            if file.lower().endswith("_" + name + ".ff") and len(file) == len(name) + 6:
                return os.path.join(folder, file)
    return None


def level_zones(level):
    """The names of the zones a level goes with and its own, in the order the game loads them."""
    zombies = level.startswith("zombie")
    return (ZOMBIES_BASE + [level + "_patch"] if zombies else SP_BASE) + [level]


def level_loads(game, level):
    """The zones to load for a level, in the order the game loads them."""
    names = level_zones(level)
    loads = []
    for name in names:
        # the language twin goes before its zone, whose references it serves
        for localized in (True, False):
            path = find_zone(game, name, localized)
            if path:
                loads.append(path)
            elif not localized and name == level:
                raise FileNotFoundError(f"the game has no zone/Common/{level}.ff")
    return loads


def write_definition(work, name, models):
    folder = os.path.join(work, "zone_source")
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, name + ".zone"), "w", encoding="utf-8", newline="\n") as f:
        f.write(f">game,T5\n>name,{name}\n\n")
        for model in models:
            f.write(f"xmodel,{model}\n")


def link_level(args, level, names):
    """Links a level's character zones in one Linker run; the names of those that were written."""
    loads = level_loads(args.game, level)
    output = os.path.join(args.work, "zone_out")
    os.makedirs(output, exist_ok=True)
    for name in names:
        built = os.path.join(output, name + ".ff")
        if os.path.exists(built):
            os.remove(built)
    command = [args.linker, "--no-color", "--base-folder", args.work, "--output-folder", output]
    for load in loads:
        command += ["--load", load]
    command += names
    started = time.monotonic()
    result = subprocess.run(command, capture_output=True, text=True, errors="replace")
    log = os.path.join(args.work, f"link-{level}.log")
    with open(log, "w", encoding="utf-8") as f:
        f.write(" ".join(f'"{part}"' for part in command) + "\n\n" + result.stdout + result.stderr)
    problems = [line for line in (result.stdout + result.stderr).splitlines()
                if "error" in line.lower() or "warning" in line.lower() or "fail" in line.lower()]
    problems = [line for line in problems if not line.startswith("Finished with 0 warnings, 0 errors")]
    written = [name for name in names if os.path.exists(os.path.join(output, name + ".ff"))]
    print(f"{level}: {len(written)}/{len(names)} zones in {time.monotonic() - started:.1f} s "
          f"(Linker exit {result.returncode}, log {log})")
    for line in problems[:20]:
        print("   " + line)
    return written


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--game", required=True, help="the game folder (with zone/Common)")
    parser.add_argument("--linker", required=True, help="OpenAssetTools' Linker.exe")
    parser.add_argument("--work", required=True, help="a folder for the definitions, logs and Linker output")
    parser.add_argument("--out", required=True, help="where the zones go (the mod's storage/t5/iris/zones)")
    parser.add_argument("--catalog", help="characters.json (see catalog.py)")
    parser.add_argument("--only", nargs="*", default=[], help="character ids to build, instead of all")
    parser.add_argument("--define", action="append", default=[], help="<zone>=<level>:<model>+<model>...")
    args = parser.parse_args()

    by_level = defaultdict(list)
    if args.define:
        for definition in args.define:
            name, rest = definition.split("=", 1)
            level, models = rest.split(":", 1)
            write_definition(args.work, name, models.split("+"))
            by_level[level].append(name)
    else:
        if not args.catalog:
            parser.error("--catalog or --define is needed")
        with open(args.catalog, encoding="utf-8") as f:
            catalog = json.load(f)
        for character in catalog["characters"]:
            if args.only and character["id"] not in args.only:
                continue
            if not character.get("zone"):
                continue
            write_definition(args.work, character["zone"], character_models(character))
            by_level[character["level"]].append(character["zone"])

    os.makedirs(args.out, exist_ok=True)
    total = failed = 0
    for level, names in sorted(by_level.items()):
        written = link_level(args, level, names)
        for name in written:
            shutil.copyfile(os.path.join(args.work, "zone_out", name + ".ff"), os.path.join(args.out, name + ".ff"))
        total += len(names)
        failed += len(names) - len(written)
        for name in sorted(set(names) - set(written)):
            print(f"   not built: {name}")
    print(f"{total - failed} of {total} character zones written to {args.out}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
