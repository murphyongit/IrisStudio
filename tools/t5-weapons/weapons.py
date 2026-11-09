"""The campaign's and Zombies' weapons for actors and the player: the catalog of the weapons
multiplayer hasn't (core/resources/campaign-weapons.json, which T5iris and Iris Studio embed) and their zones, linked
with OpenAssetTools' Linker out of the user's own levels as the character zones are: t5w_<weapon> (the weapon, its
upgraded, left-hand and alternate variants and everything they use) and t5s_<bank> (a level's sound bank, which has
the weapons' sounds). Iris Studio's setup builds the same zones (SetupLogic::ReadWeaponZones).

    python -I tools/t5-weapons/weapons.py catalog --game <game folder> --oat <OpenAssetTools folder> --work <folder>
                                          [--out core/resources/campaign-weapons.json]
    python -I tools/t5-weapons/weapons.py zones --game <game folder> --linker <Linker.exe> --work <folder>
                                          --out <folder> [--weapons campaign-weapons.json]

The catalog lists each weapon's variants (the weapon, its Pack-a-Punch one as the attachment "pap", and for the
multiplayer guns Zombies upgrades only that), their display names (the localized string, else the name below), the
zone each is in, the sound zone its shot's sound is in (found in the linked banks: an alias name is plain in a zone),
and whether its shot is a projectile. A zone's sources are the levels it can be linked from, those of the list below
first; the setup links it out of the first the game has.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, "tools", "t5-characters"))
import fastfile  # noqa: E402
import zones as character_zones  # noqa: E402

CAMPAIGN_LEVELS = ["cuba", "vorkuta", "pentagon", "flashpoint", "khe_sanh", "hue_city", "kowloon", "fullahead",
                   "creek_1", "river", "wmd_sr71", "wmd", "pow", "rebirth", "int_escape", "underwaterbase"]
ZOMBIES_MAPS = ["zombie_theater", "zombie_pentagon", "zombie_cosmodrome", "zombie_coast", "zombie_temple",
                "zombie_moon", "zombie_cod5_prototype", "zombie_cod5_asylum", "zombie_cod5_sumpf",
                "zombie_cod5_factory"]
REMAKES = ["zombie_cod5_factory", "zombie_cod5_asylum", "zombie_cod5_prototype", "zombie_cod5_sumpf"]

# (weapon, its Pack-a-Punch one or None, category, levels it is linked from first, name when the game has no string)
CAMPAIGN = [
    ("ppsh_sp", None, "Submachine Guns", ["fullahead"], "PPSh-41"),
    ("mp40_sp", None, "Submachine Guns", ["fullahead"], "MP40"),
    ("sten_sp", None, "Submachine Guns", ["fullahead"], "Sten"),
    ("stg44_sp", None, "Rifles", ["fullahead"], "STG-44"),
    ("mosin_sp", None, "Rifles", ["fullahead"], "Mosin-Nagant"),
    ("mosin_scope_sp", None, "Rifles", ["fullahead"], "Scoped Mosin-Nagant"),
    ("aug_arctic_acog_sp", None, "Rifles", ["wmd", "wmd_sr71"], "AUG Arctic"),
    ("ks23_sp", None, "Shotguns", ["vorkuta", "cuba", "flashpoint", "rebirth", "river", "wmd"], "KS-23"),
    ("ks23_hook_sp", None, "Shotguns", ["vorkuta"], "KS-23 Harpoon"),
    ("vorkuta_motorcycle_shotgun_sp", None, "Shotguns", ["vorkuta"], "Model 1887"),
    ("spas_db_sp", None, "Shotguns", ["hue_city"], "SPAS-12 Dragon's Breath"),
    ("tokarevtt30_sp", None, "Pistols", ["fullahead"], "Tokarev TT-33"),
    ("waltherp38_sp", None, "Pistols", ["fullahead"], "Walther P38"),
    ("m1911_sp", None, "Pistols", ["pentagon", "khe_sanh", "hue_city", "kowloon"], "M1911"),
    ("creek_flashlight_pistol_sp", None, "Pistols", ["creek_1"], "Python Flashlight"),
    ("fullahead_flashlight_pistol_sp", None, "Pistols", ["fullahead"], "Tokarev Flashlight"),
    ("panzerschreck_sp", None, "Launchers", ["fullahead"], "Panzerschreck"),
    ("crossbow_sp", None, "Launchers", ["flashpoint", "wmd", "wmd_sr71"], "Crossbow"),
    ("knife_karambit_sp", None, "Melee", ["flashpoint"], "Karambit"),
    ("knife_creek_sp", None, "Melee", ["creek_1"], "SOG Knife"),
    ("vorkuta_knife_sp", None, "Melee", ["vorkuta"], "Prison Knife"),
]
ZOMBIES = [
    ("ray_gun_zm", "ray_gun_upgraded_zm", "Wonder Weapons", ZOMBIES_MAPS, "Ray Gun"),
    ("thundergun_zm", "thundergun_upgraded_zm", "Wonder Weapons",
     ["zombie_theater", "zombie_cosmodrome", "zombie_cod5_prototype"], "Thundergun"),
    ("tesla_gun_zm", "tesla_gun_upgraded_zm", "Wonder Weapons",
     ["zombie_cod5_factory", "zombie_cod5_sumpf", "zombie_coast"], "Wunderwaffe DG-2"),
    ("freezegun_zm", "freezegun_upgraded_zm", "Wonder Weapons", ["zombie_pentagon", "zombie_cod5_asylum"],
     "Winter's Howl"),
    ("shrink_ray_zm", "shrink_ray_upgraded_zm", "Wonder Weapons", ["zombie_temple"], "31-79 JGb215"),
    ("humangun_zm", "humangun_upgraded_zm", "Wonder Weapons", ["zombie_coast"], "V-R11"),
    ("sniper_explosive_zm", "sniper_explosive_upgraded_zm", "Wonder Weapons", ["zombie_coast"], "Scavenger"),
    ("microwavegundw_zm", "microwavegundw_upgraded_zm", "Wonder Weapons", ["zombie_moon"], "Zap Gun Dual Wield"),
    ("mp40_zm", "mp40_upgraded_zm", "World War II", ZOMBIES_MAPS, "MP40"),
    ("zombie_thompson", "zombie_thompson_upgraded", "World War II", REMAKES, "Thompson"),
    ("zombie_type100_smg", "zombie_type100_smg_upgraded", "World War II", REMAKES, "Type 100"),
    ("zombie_stg44", "zombie_stg44_upgraded", "World War II", REMAKES, "STG-44"),
    ("zombie_fg42", "zombie_fg42_upgraded", "World War II", REMAKES, "FG42"),
    ("zombie_bar", None, "World War II", REMAKES, "BAR"),
    ("zombie_m1garand", None, "World War II", REMAKES, "M1 Garand"),
    ("zombie_m1carbine", "zombie_m1carbine_upgraded", "World War II", REMAKES, "M1 Carbine"),
    ("zombie_gewehr43", "zombie_gewehr43_upgraded", "World War II", REMAKES, "Gewehr 43"),
    ("zombie_kar98k", "zombie_kar98k_upgraded", "World War II", REMAKES, "Kar98k"),
    ("kar98k_scoped_zombie", None, "World War II", REMAKES, "Scoped Kar98k"),
    ("zombie_type99_rifle", None, "World War II", REMAKES, "Arisaka"),
    ("zombie_shotgun", "zombie_shotgun_upgraded", "World War II", REMAKES, "M1897 Trench Gun"),
    ("zombie_doublebarrel", "zombie_doublebarrel_upgraded", "World War II", REMAKES, "Double-Barreled Shotgun"),
    ("zombie_doublebarrel_sawed", None, "World War II", REMAKES, "Sawed-Off Shotgun"),
]
# multiplayer's guns that Zombies upgrades: (the multiplayer weapon, [(its variant's attachments, the upgraded one)])
UPGRADES = [
    ("mp5k_mp", [([], "mp5k_upgraded_zm")]), ("ak74u_mp", [([], "ak74u_upgraded_zm")]),
    ("pm63_mp", [([], "pm63_upgraded_zm")]), ("mpl_mp", [([], "mpl_upgraded_zm")]),
    ("spectre_mp", [([], "spectre_upgraded_zm")]), ("m16_mp", [([], "m16_gl_upgraded_zm")]),
    ("m14_mp", [([], "m14_upgraded_zm")]), ("famas_mp", [([], "famas_upgraded_zm")]),
    ("galil_mp", [([], "galil_upgraded_zm")]), ("aug_mp", [([], "aug_acog_mk_upgraded_zm")]),
    ("fnfal_mp", [([], "fnfal_upgraded_zm")]), ("commando_mp", [([], "commando_upgraded_zm")]),
    ("g11_mp", [([], "g11_lps_upgraded_zm")]), ("rottweil72_mp", [([], "rottweil72_upgraded_zm")]),
    ("ithaca_mp", [([], "ithaca_upgraded_zm")]), ("spas_mp", [([], "spas_upgraded_zm")]),
    ("hs10_mp", [([], "hs10_upgraded_zm")]), ("hk21_mp", [([], "hk21_upgraded_zm")]),
    ("rpk_mp", [([], "rpk_upgraded_zm")]), ("dragunov_mp", [([], "dragunov_upgraded_zm")]),
    ("l96a1_mp", [([], "l96a1_upgraded_zm")]), ("m1911_mp", [([], "m1911_upgraded_zm")]),
    ("python_mp", [([], "python_upgraded_zm")]),
    ("cz75_mp", [([], "cz75_upgraded_zm"), (["dw"], "cz75dw_upgraded_zm")]),
    ("m72_law_mp", [([], "m72_law_upgraded_zm")]), ("china_lake_mp", [([], "china_lake_upgraded_zm")]),
    ("crossbow_explosive_mp", [([], "crossbow_explosive_upgraded_zm")]),
    ("knife_ballistic_mp", [([], "knife_ballistic_upgraded_zm")]),
]
PAP = "pap"
# The textures of some weapons (World at War's guns, the Zap Gun, three of the Wunderwaffe's) stream from single
# player's Rezurrection files (main/iw_36.iwd to iw_41.iwd), which multiplayer's file system skips ("Skip DLC5 SP only"):
# the setup copies those textures (and only those, so nothing of multiplayer's is shadowed) into one file of
# Plutonium's storage/t5/main, which the game searches before its own main
REZURRECTION = [f"iw_{n}.iwd" for n in range(36, 42)]
WEAPONS_IWD = "iw_iris_weapons.iwd"
# Most wonder weapons do their damage in Zombies' scripts, which multiplayer doesn't run: their defs have 0 or 1.
# What T5iris writes in their defs for it instead: [direct hit, explosion at its centre, at its
# edge, its radius]. Their own radius where they have one, the Ray Gun's numbers else, a bit wider for those that hit a crowd;
# the Scavenger the numbers of the bolt its script throws (sniper_explosive_bolt_zm)
STAND_IN_DAMAGE = {
    "thundergun_zm": [1000, 1000, 200, 240], "thundergun_upgraded_zm": [1000, 1000, 200, 300],
    "tesla_gun_zm": [1000, 1000, 200, 110], "tesla_gun_upgraded_zm": [1000, 1000, 200, 150],
    "freezegun_zm": [1000, 1000, 200, 96], "freezegun_upgraded_zm": [1000, 1000, 200, 128],
    "shrink_ray_zm": [1000, 1000, 200, 64], "shrink_ray_upgraded_zm": [1000, 1000, 200, 96],
    "humangun_zm": [1000, 1000, 200, 64], "humangun_upgraded_zm": [1000, 1000, 200, 96],
    "sniper_explosive_zm": [1000, 10000, 10000, 360], "sniper_explosive_upgraded_zm": [1000, 10000, 10000, 480],
    "microwavegundw_zm": [1000, 1000, 200, 64], "microwavegundw_upgraded_zm": [1000, 1000, 200, 64],
    "microwavegunlh_zm": [1000, 1000, 200, 64], "microwavegunlh_upgraded_zm": [1000, 1000, 200, 64],
    "microwavegun_zm": [1000, 1000, 200, 128], "microwavegun_upgraded_zm": [1000, 1000, 200, 160],
}
# names in place of the game's: those it has no string for, and shorter or fixed ones of its own
NAMES = {"zombie_stg44_upgraded": "Spatz-447 +", "zombie_doublebarrel_sawed": "Sawed-Off Shotgun",
         "thundergun_upgraded_zm": "Zeus Cannon", "tesla_gun_upgraded_zm": "Wunderwaffe DG-3 JZ"}


def tool(oat, name):
    path = os.path.join(oat, name)
    if not os.path.isfile(path):
        raise SystemExit(f"OpenAssetTools' {name} isn't in {oat}")
    return path


def zone_lists(args, zone_path):
    """{type: set of names} of a zone, from the Unlinker's --list (kept in the work folder)"""
    cache = os.path.join(args.work, "lists", os.path.basename(zone_path) + ".txt")
    if not os.path.isfile(cache):
        os.makedirs(os.path.dirname(cache), exist_ok=True)
        result = subprocess.run([tool(args.oat, "Unlinker.exe"), "--no-color", "--list", zone_path],
                                capture_output=True, text=True, errors="replace")
        with open(cache, "w", encoding="utf-8") as f:
            f.write(result.stdout)
    assets = {}
    with open(cache, encoding="utf-8") as f:
        for line in f:
            if "," in line and not line.startswith(("Loaded", "Unloaded", "Zone")):
                kind, name = line.rstrip("\n").split(",", 1)
                assets.setdefault(kind.strip(), set()).add(name.strip())
    return assets


def level_assets(args, level):
    """what a level's zones and those loaded with it have, {type: set}"""
    merged = {}
    for path in character_zones.level_loads(args.game, level):
        for kind, names in zone_lists(args, path).items():
            merged.setdefault(kind, set()).update(names)
    return merged


def run_linker(linker, work, loads, names):
    output = os.path.join(work, "zone_out")
    os.makedirs(output, exist_ok=True)
    for name in names:
        built = os.path.join(output, name + ".ff")
        if os.path.exists(built):
            os.remove(built)
    command = [linker, "--no-color", "--base-folder", work, "--output-folder", output]
    for load in loads:
        command += ["--load", load]
    result = subprocess.run(command + names, capture_output=True, text=True, errors="replace")
    return result.stdout + result.stderr


def write_definition(work, name, assets):
    folder = os.path.join(work, "zone_source")
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, name + ".zone"), "w", encoding="utf-8", newline="\n") as f:
        f.write(f">game,T5\n>name,{name}\n\n" + "".join(f"{kind},{asset}\n" for kind, asset in assets))


def read_weapon_files(folder):
    """{weapon: {field: value}} of the weapon files the Unlinker wrote under a folder"""
    weapons = {}
    for root, _, files in os.walk(folder):
        if os.path.basename(root) != "weapons":
            continue
        for file in files:
            with open(os.path.join(root, file), encoding="latin-1") as f:
                parts = f.read().split("\\")
            weapons[file] = dict(zip(parts[1::2], parts[2::2]))
    return weapons


def read_localized(args, levels):
    """the English strings of the levels' language twins and the common zones, {reference: text}"""
    folder = os.path.join(args.work, "localize")
    english = os.path.join(args.game, "zone", "English")
    names = {"common", "common_mp", "common_zombie", "patch"} | set(levels)
    for name in sorted(names):
        path = os.path.join(english, "en_" + name + ".ff")
        if os.path.isfile(path) and not os.path.isfile(os.path.join(folder, "done_" + name)):
            subprocess.run([tool(args.oat, "Unlinker.exe"), "--no-color", "--include-assets", "localize", "-o",
                            folder, path], capture_output=True)
            open(os.path.join(folder, "done_" + name), "w").close()
    strings = {}
    for root, _, files in os.walk(folder):
        for file in files:
            if not file.endswith(".str"):
                continue
            reference = None
            with open(os.path.join(root, file), encoding="utf-8", errors="replace") as f:
                for line in f:
                    found = re.match(r"REFERENCE\s+(\S+)", line)
                    if found:
                        reference = found.group(1)
                    found = re.match(r'LANG_ENGLISH\s+"(.*)"', line)
                    if found and reference:
                        strings.setdefault(reference, found.group(1).replace("\\n", "").strip())
                        reference = None
    return strings


def catalog(args):
    linker = tool(args.oat, "Linker.exe")
    candidates = [level for level in CAMPAIGN_LEVELS + ZOMBIES_MAPS
                  if os.path.isfile(os.path.join(args.game, "zone", "Common", level + ".ff"))]
    available = {level: level_assets(args, level) for level in candidates}
    banks_of = {level: [] for level in candidates}  # the sound banks of a level's zones, its own first
    for level in candidates:
        for path in reversed(character_zones.level_loads(args.game, level)):
            for bank in sorted(zone_lists(args, path).get("soundbank", [])):
                if bank not in banks_of[level]:
                    banks_of[level].append(bank)

    entries = []  # (group, category, fallback, first variant, [(attachments, variant)], levels)
    for weapon, upgraded, category, levels, fallback in CAMPAIGN + ZOMBIES:
        group = "Campaign" if weapon in [row[0] for row in CAMPAIGN] else "Zombies"
        variants = [([], weapon)] + ([([PAP], upgraded)] if upgraded else [])
        entries.append((group, category, fallback, weapon, variants, levels))
    for weapon, upgrades in UPGRADES:
        entries.append(("", "", "", weapon, [(attachments + [PAP], upgraded) for attachments, upgraded in upgrades],
                        ZOMBIES_MAPS))

    # each entry's variants linked from the first level that has them, to read their files
    probes = {}
    for entry in entries:
        names = [name for _, name in entry[4]]
        level = next((level for level in entry[5] + candidates
                      if level in available and all(name in available[level].get("weapon", ()) for name in names)),
                     None)
        if not level:
            print(f"no level of this game has {', '.join(names)}: left out", file=sys.stderr)
            continue
        probes.setdefault(level, []).extend(names)
    files = {}
    for level, names in probes.items():
        name = "probe_" + level
        write_definition(args.work, name, [("weapon", n) for n in names])
        log = run_linker(linker, args.work, character_zones.level_loads(args.game, level), [name])
        dump = os.path.join(args.work, "dump", level)
        shutil.rmtree(dump, ignore_errors=True)
        subprocess.run([tool(args.oat, "Unlinker.exe"), "--no-color", "--skip-obj", "--include-assets", "weapon", "-o",
                        dump, os.path.join(args.work, "zone_out", name + ".ff")], capture_output=True)
        found = read_weapon_files(dump)
        files.update(found)
        print(f"{level}: {len(found)} weapon files ({log.strip().splitlines()[-1] if log.strip() else 'no log'})")

    # the banks (linked once each) and the aliases in them
    bank_data = {}

    def bank_has(bank, alias):
        if bank not in bank_data:
            level = next(level for level in candidates if bank in banks_of[level])
            name = "t5s_" + bank.split(".")[0]
            write_definition(args.work, name, [("soundbank", bank)])
            run_linker(linker, args.work, character_zones.level_loads(args.game, level), [name])
            path = os.path.join(args.work, "zone_out", name + ".ff")
            bank_data[bank] = fastfile.inflate(path) if os.path.isfile(path) else b""
        return (b"\0" + alias.encode() + b"\0") in bank_data[bank]

    strings = read_localized(args, candidates)
    weapons, zones, used_banks = [], [], {}
    for group, category, fallback, base, variants, levels in entries:
        if any(name not in files for _, name in variants):
            continue
        # the zone has the variants, their left-hand guns and alternate modes
        assets = []
        for _, name in variants:
            for linked in [name, files[name].get("DualWieldWeapon", ""), files[name].get("altWeapon", "")]:
                if linked and linked not in assets:
                    assets.append(linked)
        zone = "t5w_" + (base if group else variants[0][1])
        sources = [level for level in levels + candidates if level in available and
                   all(asset in available[level].get("weapon", ()) for asset in assets)]
        sources = list(dict.fromkeys(sources))
        zones.append({"zone": zone, "sources": sources, "assets": [["weapon", asset] for asset in assets]})
        entry = {"name": base, "variants": []}
        if group:
            entry.update({"display": "", "group": group, "category": category})
        for attachments, name in variants:
            fields = files[name]
            # The shot's sound: the first of its sounds a bank has (some names have no sound in any bank of the game:
            # the BAR's, the V-R11's, most upgraded multiplayer guns'), the bank of the levels it is linked from, the
            # map's own first, then the shared ones; an upgraded multiplayer gun's own sound is multiplayer's, which
            # its banks always have
            aliases = [fields.get(key, "") for key in ("fireSound", "lastShotSound", "fireSoundPlayer",
                                                        "lastShotSoundPlayer")]
            sound, bank = "", None
            for alias in filter(None, aliases if category != "Melee" else []):
                bank = next((bank for level in sources for bank in banks_of[level] if bank_has(bank, alias)), None)
                if bank:
                    sound = alias
                    break
            if not bank and not group and aliases[0].endswith("_pap"):
                sound = aliases[0][:-len("_pap")]
            sounds = ""
            if bank:
                sounds = "t5s_" + bank.split(".")[0]
                used_banks.setdefault(bank, [level for level in candidates if bank in banks_of[level]])
            if not sound:
                print(f"{name}: no bank has a sound of its shots ({', '.join(filter(None, aliases))})",
                      file=sys.stderr)
            display = (NAMES.get(name) or strings.get(fields.get("displayName", ""), "") or
                       (fallback if not attachments else name))
            variant = {"name": name, "attachments": attachments, "display": display, "zone": zone, "sound": sound,
                       "sounds": sounds, "projectile": fields.get("weaponType") == "projectile",
                       "left": fields.get("DualWieldWeapon", "") if fields.get("dualWield") == "1" else ""}
            entry["variants"].append(variant)
        if group:
            entry["display"] = entry["variants"][0]["display"]
        weapons.append(entry)
    for bank, levels in sorted(used_banks.items()):
        zones.append({"zone": "t5s_" + bank.split(".")[0], "sources": levels, "assets": [["soundbank", bank]]})

    # the textures of the zones that only Rezurrection's files have
    iwds = {}
    for path in sorted(os.listdir(os.path.join(args.game, "main"))):
        if path.lower().endswith(".iwd"):
            with zipfile.ZipFile(os.path.join(args.game, "main", path)) as z:
                for entry in z.namelist():
                    if entry.lower().startswith("images/") and entry.lower().endswith(".iwi"):
                        iwds.setdefault(os.path.splitext(os.path.basename(entry))[0].lower(), set()).add(path.lower())
    streamed = set()
    by_level = {}
    for zone in zones:
        if zone["zone"].startswith("t5w_"):
            write_definition(args.work, "check_" + zone["zone"], [tuple(asset) for asset in zone["assets"]])
            by_level.setdefault(zone["sources"][0], []).append("check_" + zone["zone"])
    for level, names in by_level.items():
        run_linker(linker, args.work, character_zones.level_loads(args.game, level), names)
    for zone in zones:
        built = os.path.join(args.work, "zone_out", "check_" + zone["zone"] + ".ff")
        if not zone["zone"].startswith("t5w_") or not os.path.isfile(built):
            continue
        for image in zone_lists(args, built).get("image", ()):
            name = image.lstrip(",").lower()
            if iwds.get(name) and iwds[name] <= set(REZURRECTION):
                streamed.add(image.lstrip(","))
                zone["rezurrection"] = True
        os.remove(os.path.join(args.work, "lists", os.path.basename(built) + ".txt"))

    # the same for the character zones: a character zone's image that can't load ends the game
    characters = {}
    if args.character_zones:
        for zone in sorted(os.listdir(args.character_zones)):
            if zone.startswith("t5c_") and zone.endswith(".ff"):
                images = zone_lists(args, os.path.join(args.character_zones, zone)).get("image", ())
                only = sorted({image.lstrip(",") for image in images
                               if iwds.get(image.lstrip(",").lower()) and iwds[image.lstrip(",").lower()] <= set(REZURRECTION)})
                if only:
                    characters[zone[:-3]] = only
                    streamed.update(only)
    out = {"version": 1, "generator": "tools/t5-weapons/weapons.py", "weapons": weapons, "zones": zones,
           "damage": STAND_IN_DAMAGE,
           "textures": {"file": WEAPONS_IWD, "images": sorted(streamed), "characters": characters}}
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(out, f, indent=1, ensure_ascii=False)
        f.write("\n")
    print(f"{len(weapons)} weapons, {len(zones)} zones ({len(used_banks)} of sounds) into {args.out}")


def build_zones(args):
    catalog = json.load(open(args.weapons, encoding="utf-8"))
    if catalog.get("version") != 1:
        raise SystemExit(f"{args.weapons} isn't of version 1")
    os.makedirs(args.out, exist_ok=True)
    by_level = {}
    for zone in catalog["zones"]:
        level = next((s for s in zone["sources"]
                      if os.path.isfile(os.path.join(args.game, "zone", "Common", s + ".ff"))), None)
        if not level:
            print(f"{zone['zone']}: the game has none of {', '.join(zone['sources'])}")
            continue
        write_definition(args.work, zone["zone"], [tuple(asset) for asset in zone["assets"]])
        by_level.setdefault(level, []).append(zone["zone"])
    failed = 0
    for level, names in by_level.items():
        log = run_linker(args.linker, args.work, character_zones.level_loads(args.game, level), names)
        with open(os.path.join(args.work, f"link-{level}.log"), "w", encoding="utf-8") as f:
            f.write(log)
        for name in names:
            built = os.path.join(args.work, "zone_out", name + ".ff")
            if os.path.isfile(built):
                shutil.copyfile(built, os.path.join(args.out, name + ".ff"))
            else:
                failed += 1
                print(f"{name}: not built (see link-{level}.log)")
        print(f"{level}: {len(names)} zones")
    if failed:
        raise SystemExit(f"{failed} weapon zones weren't built")
    if args.iwd:
        write_textures(args.game, catalog.get("textures", {}).get("images", []), args.iwd)


def write_textures(game, images, folder):
    """the textures only Rezurrection's files have, from them into one file of a folder the game searches"""
    wanted = {image.lower() for image in images}
    os.makedirs(folder, exist_ok=True)
    target = os.path.join(folder, WEAPONS_IWD)
    written = 0
    with zipfile.ZipFile(target + ".part", "w", zipfile.ZIP_STORED) as out:
        for name in REZURRECTION:
            path = os.path.join(game, "main", name)
            if not os.path.isfile(path):
                continue
            with zipfile.ZipFile(path) as z:
                for entry in z.infolist():
                    base = os.path.splitext(os.path.basename(entry.filename))[0].lower()
                    if entry.filename.lower().startswith("images/") and base in wanted:
                        out.writestr("images/" + os.path.basename(entry.filename), z.read(entry))
                        wanted.discard(base)
                        written += 1
    os.replace(target + ".part", target)
    print(f"{written} textures into {target}" + (f", {len(wanted)} not found" if wanted else ""))


def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    make = sub.add_parser("catalog")
    make.add_argument("--game", required=True)
    make.add_argument("--oat", required=True)
    make.add_argument("--work", required=True)
    make.add_argument("--out", default=os.path.join(REPO, "core", "resources", "campaign-weapons.json"))
    make.add_argument("--character-zones", help="the built character zones (t5c_*.ff), whose textures it lists too")
    build = sub.add_parser("zones")
    build.add_argument("--game", required=True)
    build.add_argument("--linker", required=True)
    build.add_argument("--work", required=True)
    build.add_argument("--out", required=True)
    build.add_argument("--weapons", default=os.path.join(REPO, "core", "resources", "campaign-weapons.json"))
    build.add_argument("--iwd", help="the folder the textures' file goes into: Plutonium's storage/t5/main")
    args = parser.parse_args()
    os.makedirs(args.work, exist_ok=True)
    if args.command == "catalog":
        catalog(args)
    else:
        build_zones(args)


if __name__ == "__main__":
    main()
