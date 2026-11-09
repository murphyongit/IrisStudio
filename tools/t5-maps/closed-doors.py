"""Puts tools/t5-maps/closed-doors.json, the doors of each campaign level and Zombies map that open into nothing and
stay closed, into core/resources/campaign-maps.json as worlds.py does (each level's
"closedDoors"), so the catalog needn't be made again for a change to the list.

The list is made by looking: each door shot from the same spot closed and open in the game (`mapoptions doors closed`,
then `mapoptions doors all`, which opens the closed ones too). What the shots show as nothing behind a door (the sky's
or the fog's colour, a hole in a floor) goes in the list. The game's collision can't tell: behind flashpoint's hall1
and under Ascension's lander pads there are clips that aren't drawn.

    python -I tools/t5-maps/closed-doors.py
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
FILE = os.path.join(HERE, "closed-doors.json")
CATALOG = os.path.join(REPO, "core", "resources", "campaign-maps.json")


def main():
    with open(FILE, encoding="utf-8") as f:
        closed = json.load(f)["levels"]
    with open(CATALOG, encoding="utf-8") as f:
        catalog = json.load(f)
    names = {entry["name"] for entry in catalog["levels"]}
    unknown = set(closed) - names
    if unknown:
        raise SystemExit(f"closed-doors.json names levels the catalog hasn't: {', '.join(sorted(unknown))}")
    for entry in catalog["levels"]:
        entry.pop("closedDoors", None)
        if closed.get(entry["name"]):
            entry["closedDoors"] = sorted(closed[entry["name"]])
            print(f"{entry['name']}: {', '.join(entry['closedDoors'])}")
    with open(CATALOG, "w", encoding="utf-8", newline="\n") as f:
        json.dump(catalog, f, separators=(",", ":"), ensure_ascii=False)
        f.write("\n")


if __name__ == "__main__":
    main()
