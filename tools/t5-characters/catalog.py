"""Writes characters.json, the catalog of the game's characters, from the single player and Zombies zones.

A character is a script in its level's zone, character/<name>.gsc, which sets the body (setModel) and attaches the
head, hat and gear (headModel, hatModel, gearModel: slots more than meanings), or picks them from the arrays of
xmodelalias/<name>.gsc scripts; it names its damage models too (torsoDmg1-4 upper bodies, legDmg1-4 lower bodies,
gibSpawn1-4 limbs). The aitype/<name>.gsc scripts that pick a character say what kind it is (human, dog, zombie) and
its team. The catalog has names only (no assets), so it can ship with the mod; tools/t5-characters/zones.py builds
the zones the mod loads the characters from.

Some models of a level no character script names: the level's own scripts set them (the field agent of Reznov's
reveal, the Zombies crew, the SR-71's ground crew). Their bodies become characters of
their own ("loose"), Shangri-La's shrunk zombies twins of its zombies, and their loose heads and hats variants of the
level's character they go with (by name, or LOOSE_PAIRS); every pairing is printed.

    python catalog.py <game folder> <characters.json>
"""
import json
import os
import re
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fastfile  # noqa: E402
import zones  # noqa: E402

# the levels, in campaign order, as the mod names them
LEVELS = [
    ("frontend", "Interrogation", "Campaign"),
    ("cuba", "Operation 40", "Campaign"),
    ("vorkuta", "Vorkuta", "Campaign"),
    ("pentagon", "U.S.D.D.", "Campaign"),
    ("flashpoint", "Executive Order", "Campaign"),
    ("khe_sanh", "S.O.G.", "Campaign"),
    ("hue_city", "The Defector", "Campaign"),
    ("kowloon", "Numbers", "Campaign"),
    ("fullahead", "Project Nova", "Campaign"),
    ("creek_1", "Victor Charlie", "Campaign"),
    ("river", "Crash Site", "Campaign"),
    ("wmd_sr71", "WMD (SR-71)", "Campaign"),
    ("wmd", "WMD", "Campaign"),
    ("pow", "Payback", "Campaign"),
    ("rebirth", "Rebirth", "Campaign"),
    ("int_escape", "Revelations", "Campaign"),
    ("underwaterbase", "Redemption", "Campaign"),
    ("outro", "Epilogue", "Campaign"),
    ("zombie_theater", "Kino der Toten", "Zombies"),
    ("zombie_pentagon", "Five", "Zombies"),
    ("zombie_cosmodrome", "Ascension", "Zombies"),
    ("zombie_coast", "Call of the Dead", "Zombies"),
    ("zombie_temple", "Shangri-La", "Zombies"),
    ("zombie_moon", "Moon", "Zombies"),
    ("zombie_cod5_prototype", "Nacht der Untoten", "Zombies"),
    ("zombie_cod5_asylum", "Verruckt", "Zombies"),
    ("zombie_cod5_sumpf", "Shi No Numa", "Zombies"),
    ("zombie_cod5_factory", "Der Riese", "Zombies"),
    ("zombietron", "Dead Ops Arcade", "Zombies"),
]

SETMODEL_RE = re.compile(r'self setModel\("([^"]+)"\)')
SETMODEL_ALIAS_RE = re.compile(r"setModelFromArray\(xmodelalias\\(\w+)::main\(\)\)")
SLOT_RE = re.compile(r'self\.(headModel|hatModel|gearModel) = "([^"]+)"')
SLOT_ALIAS_RE = re.compile(r"self\.(headModel|hatModel|gearModel) = codescripts\\character::randomElement\("
                           r"xmodelalias\\(\w+)::main\(\)\)")
ATTACH_RE = re.compile(r"self attach\(self\.(headModel|hatModel|gearModel)(, \"\", true)?\)")
DAMAGE_RE = re.compile(r'self\.(torsoDmg|legDmg|gibSpawn)(\d) = "([^"]+)"')
GIB_TAG_RE = re.compile(r'self\.gibSpawnTag(\d) = "([^"]+)"')
ALIAS_ENTRY_RE = re.compile(r'a\[\d+\] = "([^"]+)";')
AITYPE_CHARACTER_RE = re.compile(r"character\\(\w+)::main\(\)")
SLOTS = {"headModel": "head", "hatModel": "hat", "gearModel": "gear"}

# characters that are no one to put in a scene: the background crowds' low detail stand-ins
SKIPPED_RE = re.compile(r"_drones?$|_drone_")

# The models of a level no character names that are characters or their parts: those the old pack loader listed (c_*,
# not a limb, first person model or arms), with bones (the reader takes some textures' names for models: they have
# none, or 255) and not crowds' or the editor's stand-ins
LOOSE_SKIP_RE = re.compile(r"_g_|gib|viewmodel|viewhands|_vm_|_arms|_radiant|_drones?$|_drone_|^c_c$|_c$")
LOOSE_BODY_BONES = 60  # a body or full body has more bones; a head, hat or gear fewer
SHRINK_RE = re.compile(r'^\s*level\.shrink_models\["(\w+)"\]\s*=\s*"(\w+)";', re.M)
# what the names pair wrongly: a loose part to the id of the character it goes with, or None to leave it out
LOOSE_PAIRS = {
    # Brooks's head in Executive Order, on the Bowman disguise body (maps/flashpoint.gsc)
    "c_usa_jungmar_head3_nohat": "flashpoint/c_usa_blackops_bowman_disguise",
    # Nikolai's head under the Moon's pressure suit (maps/zombie_moon.gsc)
    "c_rus_nikolai_dlc5_head_psuit": "zombie_moon/c_zom_moon_pressure_suit_body_player",
    # the frozen girl of the Moon's quest is a figure, not a part; the shrunk shirt the game leaves out of its shrink
    # table (the shrunk sonic zombie has none)
    "c_zom_moon_frozen_girl": None,
    "c_viet_zombie_sonic_shirt_m": None,
}
# what a loose body's name gets wrong: the players' pressure suit isn't a zombie's
LOOSE_KINDS = {"c_zom_moon_pressure_suit_body_player": "human"}
LOOSE_NAMES = {"c_zom_moon_pressure_suit_body_player": "Pressure suit"}

# The names players know them by (the scripts call Woods "barnes"), by a part of the script's name; the rest of the
# name, less the parts that only say where the character is from, becomes what is in brackets after it.
PEOPLE = [
    ("barnes", "Woods"), ("banres", "Woods"), ("bowman", "Bowman"), ("hudson", "Hudson"), ("weaver", "Weaver"),
    ("mason", "Mason"), ("reznov", "Reznov"), ("kravchenko", "Kravchenko"), ("dragovich", "Dragovich"),
    ("steiner", "Steiner"), ("castro", "Castro"), ("carlos", "Carlos"), ("clark", "Clark"), ("jfk", "Kennedy"),
    ("mcnamara", "McNamara"), ("sergei", "Sergei"), ("patrenko", "Petrenko"), ("richtofen", "Richtofen"),
    ("takeo", "Takeo"), ("nikolai", "Nikolai"), ("dempsey", "Dempsey"), ("danny_trejo", "Danny Trejo"),
    ("george_romero", "George A. Romero"), ("michael_rooker", "Michael Rooker"),
    ("robert_englund", "Robert Englund"), ("sarah_michelle_gellar", "Sarah Michelle Gellar"),
]
# what the start of a script's name says the character is, longest first
GROUPS = [
    ("c_usa_jungmar_pow", "POW"), ("c_usa_jungmar", "Marine"), ("c_usa_huey_pilot", "Huey pilot"),
    ("c_usa_blackops", "Black Ops"), ("c_usa_specop", "Special forces"), ("c_usa_pent_ciaagent", "CIA agent"),
    ("c_usa_pent_zombie", "Zombie"), ("c_usa_pent", ""), ("c_usa_ubase", "Diver"), ("c_usa_rebirth", "Rebirth"),
    ("c_usa_interrogation", "Interrogation"), ("c_usa_cub", "Cuba"), ("c_usa", ""), ("c_vtn_civ", "Civilian"),
    ("c_vtn_nva", "NVA"), ("c_vtn_vc_pow", "Viet Cong"), ("c_vtn_vc", "Viet Cong"), ("c_viet_zombie", "Zombie"),
    ("c_rus_spetznaz", "Spetsnaz"), ("c_rus_spetnaz", "Spetsnaz"), ("c_rus_military", "Soviet soldier"),
    ("c_rus_fullahead", "Soviet"), ("c_rus_prison_guard", "Prison guard"), ("c_rus_prisoner", "Prisoner"),
    ("c_rus_scientists", "Scientists"), ("c_rus_scientist", "Scientist"), ("c_rus_engineer", "Engineer"), ("c_rus_airplane", "Gassed"),
    ("c_rus_jungmar_pow", "POW"), ("c_rus", ""), ("c_cub_tropas", "Cuban soldier"),
    ("c_cub_rebels", "Cuban rebel"), ("c_cub_police", "Cuban police"), ("c_cub_civilians", "Cuban civilian"),
    ("c_cub", ""), ("c_brt_fullahead", "British soldier"), ("c_ger_infantry", "German soldier"),
    ("c_ger_officer", "German officer"), ("c_ger_honorguard", "Zombie honor guard"), ("c_ger", ""),
    ("c_jap", ""), ("c_zom_moon", "Zombie"), ("c_zom_cosmo", "Zombie"), ("c_zom", "Zombie"),
    ("char_jap_zombie", "Zombie"), ("char_ger_engineer", "Zombie engineer"), ("character_sp_", ""),
]
# parts of names that say nothing a list of characters needs, and words written out
NOISE = {"c", "usa", "rus", "vtn", "cub", "ger", "jap", "brt", "jungmar", "ks", "khesanh", "dlc2", "dlc5", "pent",
         "zt", "sp", "fb", "body", "player", "zom", "zombies"}
WORDS = {"gas": "gas mask", "gasmask": "gas mask", "char": "burnt", "nogear": "no gear", "nobackpack": "no backpack",
         "halo": "HALO", "cqb": "CQB", "lmg": "LMG", "snip": "sniper", "mp": "MP", "nva": "NVA", "vc": "VC",
         "sr71": "SR-71", "macv": "MACV", "ciaagent": "CIA agent", "jfk": "JFK", "ubase": "base",
         "getwet": "wet", "barechest": "bare chest", "headblown": "head shot", "sheperd": "shepherd",
         "cubcasual": "casual", "cubrebel": "rebel", "specop": "special forces", "blackops": "Black Ops",
         "noglasses": "no glasses", "nohelm": "no helmet", "militarypolice": "military police",
         "officeworker": "office worker", "fullahead": "Nova", "zombietron": "Dead Ops", "divegear": "dive gear",
         "sillhouette": "silhouette", "tshirt": "T-shirt", "fieldagent": "field agent", "groundcrew": "ground crew",
         "zombiefied": "zombified", "bleeder": "bleeding", "pris": "prisoner", "psuit": "pressure suit",
         "bodya": "body A", "bodyb": "body B", "zm": "Zombies", "sog": "SOG"}


def display_name(script):
    words = script
    for part, name in PEOPLE:
        if part in script:
            rest = script.replace(part, " ")
            extra = [WORDS.get(w, w) for w in re.split(r"[_ ]+", rest) if w and w not in NOISE]
            return name + (f" ({' '.join(extra)})" if extra else "")
    label = ""
    for prefix, group in GROUPS:
        if script.startswith(prefix):
            label, words = group, script[len(prefix):]
            break
    extra = [WORDS.get(w, w) for w in re.split(r"_+", words) if w and w not in NOISE]
    # "nva1" -> "NVA 1", "undercover1a" -> "undercover 1a"
    extra = [re.sub(r"^([a-z]+)(\d+[a-z]?)$", lambda m: f"{WORDS.get(m.group(1), m.group(1))} {m.group(2)}", w)
             for w in extra]
    # what the label says already ("Zombie zombie military police", "British soldier soldier")
    said = {word.lower() for word in label.split()}
    extra = [w for w in extra if w.lower() not in said]
    text = " ".join(([label] if label else []) + extra).strip()
    return text[:1].upper() + text[1:] if text else script


base_models = {}  # the models of the zones levels go with, by zone


def read_level(game, level, path):
    """The level's character scripts, the bones of the models of it and the zones it goes with (Zombies maps take their
    characters' models from common_zombie and their own patch zone), its own models' bones and all its scripts."""
    data = fastfile.inflate(path)
    scripts = fastfile.rawfiles(data)
    bones = {name: info[0] for name, info in fastfile.models(data).items()}
    own = dict(bones)
    texts = {name: text for name, text in fastfile.rawfiles(data, ("",), tolerant=True).items()
             if name.endswith((".gsc", ".csc"))}
    for base in zones.level_zones(level)[:-1]:
        if base not in base_models:
            base_path = zones.find_zone(game, base)
            base_models[base] = ({name: info[0] for name, info in fastfile.models(fastfile.inflate(base_path)).items()}
                                 if base_path else {})
        for name, count in base_models[base].items():
            bones.setdefault(name, count)
    return scripts, bones, own, texts


def parse_level(level, scripts):
    """The level's characters, as catalog entries without their bones."""
    aliases = {name[len("xmodelalias/"):-4]: ALIAS_ENTRY_RE.findall(text)
               for name, text in scripts.items() if name.startswith("xmodelalias/")}
    kinds, teams = defaultdict(set), defaultdict(set)
    for name, text in scripts.items():
        if not name.startswith("aitype/"):
            continue
        kind = re.search(r'self\.type = "([^"]*)"', text)
        team = re.search(r'self\.team = "([^"]*)"', text)
        for script in AITYPE_CHARACTER_RE.findall(text):
            if kind:
                kinds[script].add(kind.group(1))
            if team:
                teams[script].add(team.group(1))

    characters = []
    for name, text in sorted(scripts.items()):
        if not name.startswith("character/") or name.startswith("character/clientscripts/"):
            continue
        script = name[len("character/"):-4]
        if SKIPPED_RE.search(script):
            continue
        variants = {}
        body = None
        if m := SETMODEL_RE.search(text):
            body = m.group(1)
        elif m := SETMODEL_ALIAS_RE.search(text):
            variants["body"] = aliases.get(m.group(1), [])
            body = variants["body"][0] if variants["body"] else None
        if not body:
            continue
        slots = {}
        for m in SLOT_RE.finditer(text):
            slots[SLOTS[m.group(1)]] = m.group(2)
        for m in SLOT_ALIAS_RE.finditer(text):
            slot = SLOTS[m.group(1)]
            variants[slot] = aliases.get(m.group(2), [])
            if variants[slot]:
                slots[slot] = variants[slot][0]
        # attach(model, "", true) puts a model on the body's skeleton; attach(model) alone on its own bone
        own_bone = [SLOTS[m.group(1)] for m in ATTACH_RE.finditer(text) if not m.group(2)]
        damage = {"torso": [], "legs": [], "pieces": []}
        gib_tags = dict(GIB_TAG_RE.findall(text))
        for m in DAMAGE_RE.finditer(text):
            if m.group(1) == "torsoDmg":
                damage["torso"].append(m.group(3))
            elif m.group(1) == "legDmg":
                damage["legs"].append(m.group(3))
            else:
                damage["pieces"].append({"model": m.group(3), "tag": gib_tags.get(m.group(2), "").lower()})
        kind = sorted(kinds[script])[0] if kinds.get(script) else ("dog" if "dog" in script else "human")
        characters.append({
            "id": f"{level}/{script}",
            "script": script,
            "name": display_name(script),
            "level": level,
            "kind": kind,
            "team": sorted(teams[script])[0] if teams.get(script) else "",
            "body": body,
            "head": slots.get("head"),
            "hat": slots.get("hat"),
            "gear": slots.get("gear"),
            "onOwnBone": sorted(own_bone),
            "variants": variants,
            "damage": {k: v for k, v in damage.items() if v},
        })
    return characters


def common_start(a, b):
    """How many characters two names start with alike."""
    n = 0
    while n < min(len(a), len(b)) and a[n] == b[n]:
        n += 1
    return n


def slot_of(model, slots):
    """The slot a loose part goes in: the one the catalog's characters use it in, else the one its name says."""
    if model in slots:
        return slots[model]
    if any(word in model for word in ("gear", "shirt", "vest", "bandolier")):
        return "gear"
    return "head" if "head" in model else "hat"


def aitype_of(model, texts):
    """The kind and team of the first aitype that names a model, or (None, "")."""
    for name, text in sorted(texts.items()):
        if name.startswith("aitype/") and f'"{model}"' in text:
            kind = re.search(r'self\.type = "([^"]*)"', text)
            team = re.search(r'self\.team = "([^"]*)"', text)
            return (kind.group(1) if kind else None), (team.group(1) if team else "")
    return None, ""


def find_loose(level, own, texts, named, characters, slots, log):
    """The level's loose models (LOOSE_SKIP_RE: those no character names) made characters: the shrunk twins of its
    zombies (Shangri-La's level.shrink_models), its loose bodies with the parts whose names share theirs up to _body,
    then its loose heads, hats and gear put in the variants of the level's character they go with (LOOSE_PAIRS, else the
    one whose part in that slot shares the longest start of its name), which changes characters. Returns the new
    characters; log gets a line for each pairing."""
    loose = sorted(m for m, bones in own.items()
                   if m.startswith("c_") and not LOOSE_SKIP_RE.search(m) and m not in named and 0 < bones < 255)
    if not loose:
        return []
    used = set()
    made = []

    # the shrunk twins: as the shrink ray makes them, every part with a shrunk model swapped for it, the others off
    shrink = dict(SHRINK_RE.findall("\n".join(texts.values())))
    for character in characters:
        if character["body"] not in shrink or (character["head"] and character["head"] not in shrink):
            continue
        twin = {**character, "id": character["id"] + "_shrunk", "script": character["script"] + "_shrunk",
                "name": character["name"] + " (shrunk)", "variants": {}, "damage": {}, "loose": True}
        for slot in ("body", "head", "hat", "gear"):
            twin[slot] = shrink.get(character[slot]) if character[slot] else None
        twin["onOwnBone"] = [slot for slot in character["onOwnBone"] if twin[slot]]
        for slot, models in character["variants"].items():
            minis = [shrink[m] for m in dict.fromkeys(models) if m in shrink]
            if minis:
                twin["variants"][slot] = minis
        used.update(models_of(twin))
        made.append(twin)
        log.append(f"  {level}: shrunk twin {twin['id']}: {' '.join(models_of(twin))}")

    # the bodies, each with the parts of its name (any of the level's: the river's engineers wear the heads Rebirth's
    # have), burnt with burnt ones
    for model in loose:
        if own[model] < LOOSE_BODY_BONES or model in used:
            continue
        parts = {}
        if "_body" in model:
            prefix = re.sub(r"\d+$", "", model[:model.index("_body")]) + "_"
            for part in sorted(own):
                if (part.startswith(prefix) and "_body" not in part and 0 < own[part] < LOOSE_BODY_BONES
                        and not LOOSE_SKIP_RE.search(part) and part.endswith("_char") == model.endswith("_char")
                        and (part in loose or part in named) and LOOSE_PAIRS.get(part, "") is not None):
                    parts.setdefault(slot_of(part, slots), []).append(part)
        kind, team = aitype_of(model, texts)
        if not kind:
            kind = "zombie" if level.startswith("zombie") and re.search(r"(^|_)zom(bie)?_", model) else "human"
        character = {
            "id": f"{level}/{model}", "script": model, "name": LOOSE_NAMES.get(model, display_name(model)), "level": level,
            "kind": LOOSE_KINDS.get(model, kind), "team": team, "body": model,
            "head": parts.get("head", [None])[0], "hat": parts.get("hat", [None])[0],
            "gear": parts.get("gear", [None])[0], "onOwnBone": [],
            "variants": {slot: models for slot, models in parts.items() if len(models) > 1}, "damage": {},
            "loose": True,
        }
        used.add(model)
        used.update(m for models in parts.values() for m in models)
        made.append(character)
        log.append(f"  {level}: body {character['id']} ({character['name']}, {character['kind']}): "
                   f"{' '.join(models_of(character)[1:]) or 'no parts'}")

    # the heads, hats and gear left, into the variants of the level's character they go with
    everyone = characters + made
    for model in loose:
        if model in used:
            continue
        if model in LOOSE_PAIRS and LOOSE_PAIRS[model] is None:
            log.append(f"  {level}: left out {model} (LOOSE_PAIRS)")
            continue
        slot = slot_of(model, slots)
        if model in LOOSE_PAIRS:
            target = next((c for c in everyone if c["id"] == LOOSE_PAIRS[model]), None)
            how = "LOOSE_PAIRS"
        else:
            best, target = 0, None
            for character in everyone:
                for other in [character[slot]] + character["variants"].get(slot, []):
                    if other and common_start(model, other) > best:
                        best, target = common_start(model, other), character
            how = f"{best} letters alike"
        if not target:
            log.append(f"  {level}: left out {model} (no character has a {slot} like it)")
            continue
        # (a copy: characters share the arrays of the aliases they pick from)
        variants = list(target["variants"].get(slot) or [m for m in [target[slot]] if m])
        if model not in variants:
            target["variants"][slot] = variants + [model]
        used.add(model)
        log.append(f"  {level}: {slot} {model} -> {target['id']} ({how})")
    return made


def models_of(character):
    """Every model a character can be made of."""
    models = [character["body"], character["head"], character["hat"], character["gear"]]
    for variant in character["variants"].values():
        models += variant
    damage = character.get("damage", {})
    models += damage.get("torso", []) + damage.get("legs", []) + [piece["model"] for piece in damage.get("pieces", [])]
    unique = []
    for model in models:
        if model and model not in unique:
            unique.append(model)
    return unique


def main():
    game, out = sys.argv[1], sys.argv[2]
    read = []
    for level, name, group in LEVELS:
        path = zones.find_zone(game, level)
        if not path:
            print(f"{level}: not in this game, left out")
            continue
        scripts, bones, own, texts = read_level(game, level, path)
        read.append((level, name, group, parse_level(level, scripts), bones, own, texts))

    # the models the characters name, and the slots they use them in
    named, slots = set(), {}
    for _, _, _, found, _, _, _ in read:
        for character in found:
            named.update(models_of(character))
            for slot in ("head", "hat", "gear"):
                for model in [character[slot]] + character["variants"].get(slot, []):
                    if model:
                        slots.setdefault(model, slot)

    levels, characters, log = [], [], []
    for level, name, group, found, bones, own, texts in read:
        loose = find_loose(level, own, texts, named, found, slots, log)
        found += loose
        for character in found:
            # (some zones write a model's name once for several uses, which this reader doesn't follow: those models'
            # bones are left out, and the mod counts them once loaded)
            character["bones"] = {model: bones[model] for model in models_of(character) if model in bones}
        print(f"{level}: {len(found)} characters ({len(loose)} of models the level's scripts use)")
        if found:
            levels.append({"zone": level, "name": name, "group": group})
        characters += found
    print("The models no character script names, as they were paired:")
    print("\n".join(log))

    # a character found in several levels (the same script and models) is kept once, in its first level
    kept, seen = [], {}
    for character in characters:
        key = (character["script"], tuple(models_of(character)))
        if key in seen:
            seen[key].setdefault("alsoIn", []).append(character["level"])
            continue
        seen[key] = character
        kept.append(character)

    # the charred twin of a character ("_char", the burnt variant) goes with it: in its level, else in any
    by_script = defaultdict(list)
    for character in kept:
        by_script[character["script"]].append(character)
    for character in kept:
        if not character["script"].endswith("_char"):
            continue
        bases = by_script.get(character["script"][:-len("_char")], [])
        base = next((b for b in bases if b["level"] == character["level"]), bases[0] if bases else None)
        if base and "burnt" not in base:
            base["burnt"] = character["id"]
            character["burntOf"] = base["id"]

    for character in kept:
        character["zone"] = zones.zone_name(character["id"])
        if len(character["zone"]) > 60:
            raise ValueError(f"zone name too long: {character['zone']}")
    catalog = {
        "version": 2,
        "generator": "tools/t5-characters/catalog.py",
        "levels": levels,
        "characters": kept,
    }
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(catalog, f, indent=1)
        f.write("\n")
    kinds = defaultdict(int)
    for character in kept:
        kinds[character["kind"]] += 1
    print(f"{len(kept)} characters ({len(characters) - len(kept)} more found again in other levels) written to {out}; "
          f"{dict(kinds)}; {sum(1 for c in kept if c.get('loose'))} of models the levels' scripts use; "
          f"{sum(1 for c in kept if len(c['bones']) < len(models_of(c)))} without the bones of a model")


if __name__ == "__main__":
    main()
