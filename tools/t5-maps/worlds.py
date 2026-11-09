"""Writes core/resources/campaign-maps.json and core/resources/campaign-kits.json from
the game's own zones, with OpenAssetTools.

campaign-maps.json, for each campaign level and Zombies map (mode "zombies", with the zones
single player loads before it that it takes assets of, common_zombie): what its world is made of (the gfxworld,
clipmap, comworld, gameworldsp and mapents, and the models its entities name, linked by OAT's Linker into a scratch
zone, whose list is the closure:
models, materials, technique sets, images, physics presets), what of that multiplayer's base zones don't have (the
room it takes), the players' teamset of its setting, its art script's first fog and vision, and its
ambient effects: the oneshot and loop effects its createfx places (not the exploders, which
its script sets off), whose effect definitions are linked with the world; and its doors that open into nothing, kept
closed (closed-doors.json beside this, found by looking at each door open in the game).
T5iris registers only those assets of the level's zone when it loads as a multiplayer map (t5/src/CampaignMaps.cpp)
and plays the effects.

campaign-kits.json, for each teamset: the players' kit a campaign map loads with (t5m_kit_<teamset>), what multiplayer
maps carry beside their world: the body, head and dog scripts, the teamset's player bodies, heads and arms, the RC-XD,
the bomb and the HQ crate, the RC-XD's effects and the teamset's faction icons, and the multiplayer maps of that
teamset it is linked out of (the first the game has). The setup builds those zones (tools/t5-maps/kits.py, the
launcher).

    python -I tools/t5-maps/worlds.py --game <Black Ops folder> --oat <folder of Linker.exe> --work <scratch folder>
                                      [level...]

Takes a few minutes; the work folder keeps the lists and logs (about 50 MB of dumps).
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RESOURCES = os.path.join(REPO, "core", "resources")

# in campaign order, with the teamset of their setting: urban spec ops, jungle marines, winter spec ops, Cubans
LEVELS = [
    ("frontend", "Interrogation", "urbanspecops"),
    ("cuba", "Operation 40", "cubans"),
    ("vorkuta", "Vorkuta", "winterspecops"),
    ("pentagon", "U.S.D.D.", "urbanspecops"),
    ("flashpoint", "Executive Order", "urbanspecops"),
    ("khe_sanh", "S.O.G.", "junglemarines"),
    ("hue_city", "The Defector", "junglemarines"),
    ("kowloon", "Numbers", "urbanspecops"),
    ("fullahead", "Project Nova", "winterspecops"),
    ("creek_1", "Victor Charlie", "junglemarines"),
    ("river", "Crash Site", "junglemarines"),
    ("wmd_sr71", "WMD (SR-71)", "winterspecops"),
    ("wmd", "WMD", "winterspecops"),
    ("pow", "Payback", "junglemarines"),
    ("rebirth", "Rebirth", "urbanspecops"),
    ("int_escape", "Revelations", "urbanspecops"),
    ("underwaterbase", "Redemption", "urbanspecops"),
    ("outro", "Epilogue", "urbanspecops"),
]
# the Zombies maps, with the teamset of their setting the same way: snow and ice winter spec
# ops, jungle and swamp jungle marines, the rest urban spec ops. Not Dead Ops Arcade (zombietron), a top-down mode the
# owner left out ("it's not a typical map", 2026-11-01; multiplayer stops on its player animations file)
ZOMBIES_LEVELS = [
    ("zombie_theater", "Kino der Toten", "urbanspecops"),
    ("zombie_pentagon", "Five", "urbanspecops"),
    ("zombie_cosmodrome", "Ascension", "urbanspecops"),
    ("zombie_coast", "Call of the Dead", "winterspecops"),
    ("zombie_temple", "Shangri-La", "junglemarines"),
    ("zombie_moon", "Moon", "urbanspecops"),
    ("zombie_cod5_prototype", "Nacht der Untoten", "urbanspecops"),
    ("zombie_cod5_asylum", "Verruckt", "urbanspecops"),
    ("zombie_cod5_sumpf", "Shi No Numa", "junglemarines"),
    ("zombie_cod5_factory", "Der Riese", "winterspecops"),
]
# each map's row in the game's Change Map menu: the game's language keys
# of its name as the list shows it (capitals) and of its description, and its picture, an image of a single player
# zone (the campaign's mission select pictures and the Zombies map select's), the order being LEVELS' and
# ZOMBIES_LEVELS'. None where the game has none: our own words then (OWN_TEXTS), and no picture for outro
MENU = {
    "frontend": (None, None, ("frontend", "int_escape")),  # Revelations' picture is of the same room
    "cuba": ("MENU_LEVEL_CUBA_CAPS", "MENU_LEVEL_CUBA_DESC", ("frontend", "cuba")),
    "vorkuta": ("MENU_LEVEL_VORKUTA_CAPS", "MENU_LEVEL_VORKUTA_DESC", ("frontend", "vorkuta")),
    "pentagon": ("MENU_LEVEL_PENTAGON_CAPS", "MENU_LEVEL_PENTAGON_DESC", ("frontend", "pentagon")),
    "flashpoint": ("MENU_LEVEL_FLASHPOINT_CAPS", "MENU_LEVEL_FLASHPOINT_DESC", ("frontend", "flashpoint")),
    "khe_sanh": ("MENU_LEVEL_KHE_SANH_CAPS", "MENU_LEVEL_KHE_SANH_DESC", ("frontend", "khe_sanh")),
    "hue_city": ("MENU_LEVEL_HUE_CITY_CAPS", "MENU_LEVEL_HUE_CITY_DESC", ("frontend", "hue_city")),
    "kowloon": ("MENU_LEVEL_KOWLOON_CAPS", "MENU_LEVEL_KOWLOON_DESC", ("frontend", "kowloon")),
    "fullahead": ("MENU_LEVEL_FULLAHEAD_CAPS", "MENU_LEVEL_FULLAHEAD_DESC", ("frontend", "fullahead")),
    "creek_1": ("MENU_LEVEL_CREEK_1_CAPS", "MENU_LEVEL_CREEK_1_DESC", ("frontend", "creek_1")),
    "river": ("MENU_LEVEL_RIVER_CAPS", "MENU_LEVEL_RIVER_DESC", ("frontend", "river")),
    "wmd_sr71": (None, None, ("frontend", "wmd")),  # WMD's first half
    "wmd": ("MENU_LEVEL_WMD_CAPS", "MENU_LEVEL_WMD_DESC", ("frontend", "wmd")),
    "pow": ("MENU_LEVEL_POW_CAPS", "MENU_LEVEL_POW_DESC", ("frontend", "pow")),
    "rebirth": ("MENU_LEVEL_REBIRTH_CAPS", "MENU_LEVEL_REBIRTH_DESC", ("frontend", "rebirth")),
    "int_escape": ("MENU_LEVEL_INTERROGATION_ESCAPE_CAPS", "MENU_LEVEL_INTERROGATION_ESCAPE_DESC",
                   ("frontend", "int_escape")),
    "underwaterbase": ("MENU_LEVEL_UNDERWATERBASE_CAPS", "MENU_LEVEL_UNDERWATERBASE_DESC",
                       ("frontend", "underwaterbase")),
    "outro": (None, None, None),
    "zombie_theater": ("MENU_ZOMBIE_THEATER_CAPS", "MENU_ZOMBIE_THEATER_DESC", ("frontend", "zombie_theater")),
    "zombie_pentagon": ("MENU_ZOMBIE_PENTAGON_CAPS", "MENU_ZOMBIE_PENTAGON_DESC", ("frontend", "zombie_pentagon")),
    "zombie_cosmodrome": ("MENU_ZOMBIE_COSMODROME_CAPS", "MENU_ZOMBIE_COSMODROME_DESC", ("patch", "zombie_cosmo")),
    "zombie_coast": ("MENU_ZOMBIE_COAST_CAPS", "MENU_ZOMBIE_COAST_DESC", ("patch", "zombie_coast")),
    "zombie_temple": ("MENU_ZOMBIE_TEMPLE_CAPS", "MENU_ZOMBIE_TEMPLE_DESC", ("patch", "zombie_temple")),
    "zombie_moon": ("MENU_ZOMBIE_MOON_CAPS", "MENU_ZOMBIE_MOON_DESC", ("patch", "zombie_moon")),
    "zombie_cod5_prototype": ("WAW_MENU_ZOMBIE_PROTOTYPE_CAPS", "WAW_MENU_ZOMBIE_PROTOTYPE_DESC",
                              ("patch", "loadscreen_zombie_prototype_mini")),
    "zombie_cod5_asylum": ("WAW_MENU_ZOMBIE_ASYLUM_CAPS", "WAW_MENU_ZOMBIE_ASYLUM_DESC",
                           ("patch", "loadscreen_zombie_asylum_mini")),
    "zombie_cod5_sumpf": ("WAW_MENU_ZOMBIE_SUMPF_CAPS", "WAW_MENU_ZOMBIE_SUMPF_DESC",
                          ("patch", "loadscreen_zombie_sumpf_mini")),
    "zombie_cod5_factory": ("WAW_MENU_ZOMBIE_FACTORY_CAPS", "WAW_MENU_ZOMBIE_FACTORY_DESC",
                            ("patch", "loadscreen_zombie_factory_mini")),
}
# our words for the three the game names nowhere (a few words), as the keys IRIS_MAP_<LEVEL>_CAPS and _DESC
OWN_TEXTS = {
    "frontend": ("INTERROGATION", "The interrogation room."),
    "wmd_sr71": ("SR-71", "WMD's first half: the SR-71's airfield."),
    "outro": ("EPILOGUE", "The epilogue's stage."),
}
# the language zones multiplayer loads (their keys need no text of ours) and single player's, whose texts are given for
# the keys multiplayer lacks (Moon's and the World at War remakes')
MP_LANGUAGE_ZONES = ["en_code_pre_gfx_mp", "en_code_post_gfx_mp", "en_common_mp", "en_ui_mp", "en_patch_mp"]
SP_LANGUAGE_ZONES = ["en_code_post_gfx", "en_patch"]
# the multiplayer maps of each teamset (their maps/mp/<map>.gsc calls its level_init), the kit's source first
KIT_SOURCES = {
    "urbanspecops": ["mp_nuked", "mp_cosmodrome", "mp_radiation"],
    "junglemarines": ["mp_havoc", "mp_hanoi", "mp_cracked"],
    "winterspecops": ["mp_array", "mp_mountain"],
    "cubans": ["mp_cairo", "mp_villa", "mp_crisis", "mp_firingrange"],
}
# what of a multiplayer map's zone the kit holds
KIT_ASSETS = [
    ("rawfile", re.compile(r"^(aitype|mpbody|mphead)/")),
    ("xmodel", re.compile(r"^(c_\w+_mp_(body|head)_\w+|viewmodel_\w+_arms|t5_veh_rcbomb_(allies|axis)|prop_suitcase_bomb|"
                          r"mp_supplydrop_hq)$")),
    ("fx", re.compile(r"^(vehicle/treadfx/fx_treadfx_rcbomb_\w+|vehicle/light/fx_rcbomb_\w+|"
                      r"weapon/grenade/fx_spark_disabled_rc_car)$")),
    # the teamset's two faction icons, which the scripts ask for by name
    ("material", re.compile(r"^faction_128_\w+$")),
]
# the zones single player loads before a level of a mode (Zombies maps share common_zombie's materials, technique sets
# and effects), which the mod loads with the level when its zone refers to them
SHARED_ZONES = {"zombies": ["common_zombie"]}
# of a kit's models, the players' bodies, heads and arms
PLAYER_MODELS = re.compile(r"^(c_\w+_mp_(body|head)_\w+|viewmodel_\w+_arms)$")
# multiplayer's zones loaded before any map, and their language twins
BASE_ZONES = ["code_pre_gfx_mp", "code_post_gfx_mp", "patch_mp", "common_mp"]
WORLD_TYPES = ["image", "material", "techniqueset", "xmodel", "physpreset", "fx"]


def find_zone(game, name):
    """zone/Common/<name>.ff, and its language twin (zone/English/en_<name>.ff), the twin first"""
    found = []
    folder = os.path.join(game, "zone")
    for language in sorted(os.listdir(folder)):
        if language.lower() == "common" or not os.path.isdir(os.path.join(folder, language)):
            continue
        for file in sorted(os.listdir(os.path.join(folder, language))):
            if file.lower().endswith("_" + name + ".ff") and len(file) == len(name) + 6:
                found.append(os.path.join(folder, language, file))
    common = os.path.join(folder, "Common", name + ".ff")
    if os.path.isfile(common):
        found.append(common)
    return found


def run(command, log=None):
    result = subprocess.run(command, capture_output=True, text=True, errors="replace")
    if log:
        with open(log, "w", encoding="utf-8") as f:
            f.write(result.stdout + result.stderr)
    return result


def read_list(path, references=True):
    """an OAT list: the names by type; a leading comma marks a reference to an asset of another zone (counted as the
    zone's, as the game registers its name too), which can't be linked out of it"""
    names = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if ", " not in line or line.startswith(("Loaded", "Unloaded", "Zone", "#")):
                continue
            kind, name = line.rstrip("\n").split(", ", 1)
            if references or not name.startswith(","):
                names.setdefault(kind, set()).add(name.lstrip(","))
    return names


def list_zone(args, path, out, references=True):
    if not os.path.isfile(out):
        listing = run([os.path.join(args.oat, "Unlinker.exe"), "--list", path]).stdout
        with open(out, "w", encoding="utf-8") as f:
            f.write(listing)
    return read_list(out, references)


def dump(args, level):
    folder = os.path.join(args.work, "dump", level)
    if not os.path.isdir(folder):
        run([os.path.join(args.oat, "Unlinker.exe"), "--include-assets", "rawfile,mapents", "-o",
             os.path.join(args.work, "dump", "?zone?"), os.path.join(args.game, "zone", "Common", level + ".ff")])
    return folder


def entity_models(args, level):
    path = os.path.join(dump(args, level), "maps", level + ".d3dbsp.ents")
    if not os.path.isfile(path):
        return set()
    text = open(path, encoding="latin-1").read()
    return {m for m in re.findall(r'"(?:model|skyboxmodel)" "([^"]+)"', text) if not m.startswith("*")}


FUNCTION = re.compile(r"^(\w+)\s*\([^)]*\)\s*\{", re.MULTILINE)
NUMBER = r"-?[\d.]+(?:e-?\d+)?"
# the levels whose main() sets no fog: the art script's function their mission notifies first (wmd_intro's
# set_runway_fog, wmd_snow_hide's set_ambush_fog)
FIRST_FOG = {"wmd_sr71": "set_runway_fog", "wmd": "set_ambush_fog"}


def functions(text):
    """a script's functions in order: (name, its text up to the next function)"""
    starts = [(m.group(1), m.start()) for m in FUNCTION.finditer(text)]
    return [(name, text[start:starts[i + 1][1] if i + 1 < len(starts) else len(text)])
            for i, (name, start) in enumerate(starts)]


def volfog(body):
    """the arguments of a function's first setVolFog with the values its variables were given before it in that
    function: None without a call, "" when one isn't a number"""
    call = re.search(r"setVolFog\s*\(([^;]*)\)\s*;", body, re.IGNORECASE)
    if not call:
        return None
    values = dict(re.findall(rf"^\s*(\w+)\s*=\s*({NUMBER})\s*;", body[:call.start()], re.MULTILINE))
    resolved = [values.get(a.strip(), a.strip()) for a in call.group(1).replace("\n", " ").split(",")]
    return ", ".join(resolved) if all(re.fullmatch(NUMBER, a) for a in resolved) else ""


def art(args, level, have, zombies):
    """the fog and vision a level starts with, from its art script read without comments (art scripts keep old calls
    commented out): main()'s first setVolFog with its variables as main() set them, else the first fog of its mission
    (FIRST_FOG), else the first setVolFog of a function after main(); and main()'s first VisionSetNaked's vision. On a
    Zombies map whose art script sets none, the vision its script gives Zombies mode (level.zombie_visionset, which
    _zombiemode sets as players connect), when the level's zone or multiplayer has it (have: the rawfiles' names)"""
    folder = dump(args, level)
    path = os.path.join(folder, "maps", "createart", level + "_art.gsc")
    script = os.path.join(folder, "maps", level + ".gsc")
    text = uncommented(path)
    body = text[text.find("main()"):] if text else ""
    after = functions(body)
    fog = volfog(after[0][1]) if after else None
    if fog is None and level in FIRST_FOG:
        fog = volfog(dict(after).get(FIRST_FOG[level], ""))
    if fog is None:
        fog = next((f for f in (volfog(text) for _, text in after[1:]) if f is not None), None)
    fog = fog or ""
    vision = re.search(r'VisionSetNaked\s*\(\s*"([^"]+)"', body, re.IGNORECASE)
    if vision or not zombies:
        return fog, vision.group(1) if vision else ""
    visionset = re.search(r'level\.zombie_visionset\s*=\s*"([^"]+)"', uncommented(script))
    return fog, visionset.group(1) if visionset and f"vision/{visionset.group(1)}.vision" in have else ""


def language(args, zone):
    """the texts of a language zone's localize entries (zone/English/<zone>.ff), by key"""
    folder = os.path.join(args.work, "language", zone)
    path = os.path.join(args.game, "zone", "English", zone + ".ff")
    if not os.path.isdir(folder) and os.path.isfile(path):
        run([os.path.join(args.oat, "Unlinker.exe"), "--include-assets", "localize", "-o", folder, path])
    texts = {}
    for root, _, files in os.walk(folder):
        for file in files:
            if file.endswith(".str"):
                text = open(os.path.join(root, file), encoding="latin-1").read()
                for key, value in re.findall(r'REFERENCE\s+(\S+)\s*\n\s*LANG_ENGLISH\s+"(.*)"', text):
                    texts[key.upper()] = value.replace('\\"', '"').replace("\\n", "\n").strip()
    return texts


def menu_entries(args):
    """each map's Change Map entry (MENU) and the texts the game's multiplayer language files don't have: those of
    single player's for its keys, ours (OWN_TEXTS) for the three it has none for"""
    multiplayer, single = {}, {}
    for zone in MP_LANGUAGE_ZONES:
        multiplayer.update(language(args, zone))
    for zone in SP_LANGUAGE_ZONES:
        single.update(language(args, zone))
    entries, strings = {}, {}
    for order, (level, _, _) in enumerate(LEVELS + ZOMBIES_LEVELS):
        name, description, picture = MENU[level]
        if name is None:
            name, description = f"IRIS_MAP_{level.upper()}_CAPS", f"IRIS_MAP_{level.upper()}_DESC"
            strings[name], strings[description] = OWN_TEXTS[level]
        for key in (name, description):
            if key not in multiplayer and key not in strings:
                if key not in single:
                    raise SystemExit(f"{level}: the game's language files have no {key}")
                strings[key] = single[key]
        entries[level] = {"order": order, "name": name, "description": description,
                          "picture": "/".join(picture) if picture else ""}
    return entries, strings


def uncommented(path):
    """a script's text without its comments, empty when the zone has none of that name"""
    if not os.path.isfile(path):
        return ""
    text = open(path, encoding="latin-1").read()
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)


CREATED = re.compile(r'::create(OneshotEffect|LoopEffect)\(\s*"([^"]+)"\s*\)(.*?)(?=::create|\Z)', re.S)
CREATED_KEY = re.compile(r'ent\.v\[\s*"(\w+)"\s*\]\s*=\s*([^;]+);')
LOADFX = re.compile(r'level\._effect\[\s*"([^"]+)"\s*\]\s*=\s*loadfx\s*\(\s*"([^"]+)"\s*\)', re.IGNORECASE)


def numbers(text):
    return [float(v) for v in re.findall(r"-?[\d.]+(?:e-?\d+)?", text)]


def createfx(args, level, zone, base, shared):
    """the ambient effects the level's createfx places (clientscripts/createfx/*.csc: oneshotfx and loopfx), each by
    the definition its fxid loads (level._effect[...] = loadfx(...) in the level's scripts): [definition, x, y, z,
    pitch, yaw, roll, delay, loop]; those whose definition neither the level's zone nor multiplayer has are left out"""
    folder = dump(args, level)
    loaded = {}
    created = []
    for root, _, files in os.walk(folder):
        for file in sorted(files):
            if not file.endswith((".csc", ".gsc")):
                continue
            text = open(os.path.join(root, file), encoding="latin-1").read()
            for fxid, definition in LOADFX.findall(text):
                loaded.setdefault(fxid, definition)
            if "createfx" in root and file.endswith(".csc"):
                created += CREATED.findall(text)
    have = zone.get("fx", set()) | base.get("fx", set()) | shared.get("fx", set())
    placed, missing = [], set()
    for kind, fxid, body in created:
        keys = dict(CREATED_KEY.findall(body))
        definition = loaded.get(fxid)
        origin, angles = numbers(keys.get("origin", "")), numbers(keys.get("angles", "0 0 0"))
        if not definition or definition not in have or len(origin) != 3 or len(angles) != 3:
            missing.add(definition or fxid)
            continue
        delay = numbers(keys.get("delay", "-15" if kind == "OneshotEffect" else "0.5"))
        placed.append([definition] + [round(v, 2) for v in origin + angles] +
                      [round(delay[0] if delay else 0.0, 2), 1 if kind == "LoopEffect" else 0])
    return placed, sorted(missing)


def link_world(args, level, effects=(), shared=()):
    """the list of a zone linked of the level's world, its entities' models and its effects' definitions, with the
    zones it shares with its mode (shared) loaded before it, as the game loads them"""
    out_list = os.path.join(args.work, "worlds", level + ".txt")
    if os.path.isfile(out_list):
        return read_list(out_list)
    zone = list_zone(args, os.path.join(args.game, "zone", "Common", level + ".ff"),
                     os.path.join(args.work, "lists", level + ".txt"), references=False)
    name = "t5w_" + level
    lines = [f"{kind},maps/{level}.d3dbsp" for kind in ("gfxworld", "clipmap", "comworld", "gameworldsp", "mapents")]
    # (an entity can name its model with capitals where the zone has none: 89 entities, as dest_rus_console_lg_A_d0)
    named = {m.lower() for m in entity_models(args, level)}
    lines += [f"xmodel,{m}" for m in sorted(m for m in zone.get("xmodel", set()) if m.lower() in named)]
    linkable = set(zone.get("fx", set()))
    for z in shared:
        linkable |= list_zone(args, os.path.join(args.game, "zone", "Common", z + ".ff"),
                              os.path.join(args.work, "lists", z + ".txt"), references=False).get("fx", set())
    lines += [f"fx,{e}" for e in sorted(set(effects) & linkable)]
    work = os.path.join(args.work, "link")
    os.makedirs(os.path.join(work, "zone_source"), exist_ok=True)
    os.makedirs(os.path.join(work, "zone_out"), exist_ok=True)
    with open(os.path.join(work, "zone_source", name + ".zone"), "w", newline="\n") as f:
        f.write(f">game,T5\n>name,{name}\n\n" + "\n".join(lines) + "\n")
    command = [os.path.join(args.oat, "Linker.exe"), "--no-color", "--base-folder", work, "--output-folder",
               os.path.join(work, "zone_out")]
    for z in ("patch", "common", *shared, level):
        for path in find_zone(args.game, z):
            command += ["--load", path]
    command.append(name)
    run(command, os.path.join(args.work, "logs", "link-" + level + ".log"))
    built = os.path.join(work, "zone_out", name + ".ff")
    if not os.path.isfile(built):
        raise SystemExit(f"{level}: the world wasn't linked (logs/link-{level}.log)")
    listing = run([os.path.join(args.oat, "Unlinker.exe"), "--list", built]).stdout
    os.remove(built)
    with open(out_list, "w", encoding="utf-8") as f:
        f.write(listing)
    return read_list(out_list)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--game", required=True)
    parser.add_argument("--oat", required=True)
    parser.add_argument("--work", required=True)
    parser.add_argument("levels", nargs="*")
    args = parser.parse_args()
    for sub in ("dump", "lists", "worlds", "logs"):
        os.makedirs(os.path.join(args.work, sub), exist_ok=True)
    menu, strings = menu_entries(args)
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "closed-doors.json"), encoding="utf-8") as f:
        closed_doors = json.load(f)["levels"]

    base = {}
    for zone in BASE_ZONES:
        for path in find_zone(args.game, zone):
            listed = list_zone(args, path, os.path.join(args.work, "lists", os.path.basename(path)[:-3] + ".txt"))
            for kind, names in listed.items():
                base.setdefault(kind, set()).update(names)

    levels = []
    for (level, title, teamset), mode in [(entry, "campaign") for entry in LEVELS] + [(entry, "zombies") for entry in
                                                                                      ZOMBIES_LEVELS]:
        if args.levels and level not in args.levels:
            continue
        if not os.path.isfile(os.path.join(args.game, "zone", "Common", level + ".ff")):
            raise SystemExit(f"{level}: the game folder has no zone/Common/{level}.ff (a DLC map it lacks)")
        started = time.monotonic()
        zone = list_zone(args, os.path.join(args.game, "zone", "Common", level + ".ff"),
                         os.path.join(args.work, "lists", level + ".txt"), references=False)
        # the zones its mode loads before it that it takes assets of (common_zombie: Zombies maps refer to its
        # materials, technique sets and effects), which the mod loads with it too
        references = list_zone(args, os.path.join(args.game, "zone", "Common", level + ".ff"),
                               os.path.join(args.work, "lists", level + ".txt"))
        shared, sharedNames = [], {}
        for z in SHARED_ZONES.get(mode, []):
            listed = list_zone(args, os.path.join(args.game, "zone", "Common", z + ".ff"),
                               os.path.join(args.work, "lists", z + ".txt"), references=False)
            if any((references.get(kind, set()) - zone.get(kind, set())) & names for kind, names in listed.items()):
                shared.append(z)
                for kind, names in listed.items():
                    sharedNames.setdefault(kind, set()).update(names)
        placed, missing = createfx(args, level, zone, base, sharedNames)
        world = link_world(args, level, [p[0] for p in placed], shared)
        fog, vision = art(args, level, zone.get("rawfile", set()) | base.get("rawfile", set()), mode == "zombies")
        new = {kind: len(world.get(kind, set()) - base.get(kind, set())) for kind in ("image", "material", "xmodel",
                                                                                         "fx")}
        names = sorted({p[0] for p in placed})
        entry = {"name": level, "title": title, "mode": mode, "teamset": teamset, "zones": shared, "fog": fog,
                 "vision": vision, "new": new, "world": {kind: sorted(world.get(kind, set())) for kind in WORLD_TYPES},
                 "effects": {"names": names, "placed": [[names.index(p[0])] + p[1:] for p in placed]},
                 "menu": menu[level]}
        if not shared:
            del entry["zones"]
        if closed_doors.get(level):
            entry["closedDoors"] = sorted(closed_doors[level])
        levels.append(entry)
        print(f"{level}: images {len(world.get('image', ()))} ({new['image']} new), materials "
              f"{len(world.get('material', ()))} ({new['material']}), models {len(world.get('xmodel', ()))} "
              f"({new['xmodel']}), effects {len(world.get('fx', ()))} ({new['fx']}), {len(placed)} placed of {len(names)} "
              f"kinds ({len(missing)} left out); fog {'yes' if fog else 'no'}, vision {vision or 'none'}"
              f"{', with ' + ', '.join(shared) if shared else ''}; "
              f"{time.monotonic() - started:.1f} s", flush=True)

    kits = []
    for teamset, sources in KIT_SOURCES.items():
        source = next(s for s in sources if os.path.isfile(os.path.join(args.game, "zone", "Common", s + ".ff")))
        listed = list_zone(args, os.path.join(args.game, "zone", "Common", source + ".ff"),
                           os.path.join(args.work, "lists", source + ".txt"))
        assets = []
        for kind, pattern in KIT_ASSETS:
            assets += [[kind, name] for name in sorted(listed.get(kind, set())) if pattern.match(name)]
        kits.append({"teamset": teamset, "zone": "t5m_kit_" + teamset, "sources": sources, "assets": assets})
        print(f"kit {teamset}: {len(assets)} assets from {source}")

    with open(os.path.join(RESOURCES, "campaign-maps.json"), "w", encoding="utf-8", newline="\n") as f:
        # the players' bodies, heads and arms of each kit, which a level's stand-in script precaches (multiplayer's
        # _teams::setPlayerModels precaches the jungle marines' on a map mp/mapsTable.csv doesn't name)
        players = {kit["teamset"]: [name for kind, name in kit["assets"]
                                    if kind == "xmodel" and PLAYER_MODELS.match(name)] for kit in kits}
        json.dump({"version": 1, "generator": "tools/t5-maps/worlds.py", "levels": levels, "players": players,
                   "strings": strings}, f, separators=(",", ":"), ensure_ascii=False)
        f.write("\n")
    with open(os.path.join(RESOURCES, "campaign-kits.json"), "w", newline="\n") as f:
        # the pictures of the Change Map menu, which the setup copies out of the game's
        # single player zones as DDS files (tools/t5-maps/pictures.py, SetupLogic::ReadMenuPictures)
        pictures = sorted({entry["picture"] for entry in menu.values() if entry["picture"]})
        json.dump({"version": 1, "generator": "tools/t5-maps/worlds.py", "kits": kits,
                   "pictures": [{"zone": p.split("/")[0], "image": p.split("/")[1]} for p in pictures]}, f, indent=1)
        f.write("\n")
    print("written", os.path.join(RESOURCES, "campaign-maps.json"), "and campaign-kits.json")


main()
