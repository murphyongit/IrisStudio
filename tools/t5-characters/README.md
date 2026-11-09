# T5 character zones

The launcher (`launcher/`, `IrisStudio.exe`) does what `setup.ps1` does in C++, without Python: `zones.py`'s zones and
`animations.py pack`'s pack, checked to come out the same. These scripts stay the
reference, and the tools for the catalogs.

Tools that make the Black Ops (T5) campaign and Zombies characters available to the mod's actors,
and every animation of the game. They read the user's own game files and write nothing
into the game folder.

- `fastfile.py`: reads a PC fastfile offline (`IWffu100`, version `0x1D9`, one zlib stream): asset counts, the text of
  the `character/`, `aitype/` and `xmodelalias/` scripts, and the models' bone counts.
  `python fastfile.py <zone.ff> [scripts [prefix...] | models [filter]]`
- `catalog.py`: writes `characters.json` (committed as `core/resources/characters.json`, which the mod embeds), the
  catalog of every character of the levels: its body, head, hat, gear, the alias arrays its parts are picked from,
  its damage models, its kind and team (from the aitypes), its charred twin, the bones of its models and the zone it
  is built into. Models no character script names, which the levels' own scripts use (the field agent of The
  Defector, the Zombies crew, Shangri-La's shrunk zombies), are characters too (`"loose": true`), their loose heads and
  hats variants of the character they go with; it prints every pairing.
  `python catalog.py <game folder> <characters.json>` (about 45 s).
- `zones.py`: builds one zone per character with OpenAssetTools' Linker (`>game,T5`, one `xmodel` line a model), all of
  a level's characters in one Linker run. The Linker loads the level with the zones it goes with first (the language
  twin `zone/<language>/en_<level>.ff`, `patch.ff`, `common.ff`; for Zombies `common_zombie` and the map's patch zone),
  since a level references some of its materials from them (Barnes's gloves, the plain marine bodies), and copies
  everything the models use into the character zone.
  `python zones.py --game <game folder> --linker <Linker.exe> --work <folder> --out <folder> --catalog characters.json`
  (`--only <id>...` for some; `--define <zone>=<level>:<model>+<model>` for a zone of your own).

- `animations.py`: the catalog of every animation of the game and the animation pack
 (step 5), read from the zones offline, without OpenAssetTools.
  `python animations.py catalog <game folder> <animations.json>` writes the catalog, committed as
  `core/resources/animations.json` (the mod embeds it; about 2 MB, 15 s): every animation of the zones but the first
  person arms' (`viewmodel_*`), with its zones, frames, rate, loop, delta, what it moves and the rig it was made for
  (as `PoseLogic::ClassifyAnimation`), notetracks, its bones (as a shared bone set), size and hash, its root's motion
  and turn in a play (decoded from its delta part: the game measures the same), and the states it starts and ends in
  (posture and movement, from SP's `animscripts/anims_table.gsc`, MP's `mp/playeranim.script`, the `anim_pose`/
  `anim_movement` notetracks and the name). Names in several zones are one entry where their data is the same; the 31
  that differ are entries of their own, `<name> (<zone>)`, the name kept by multiplayer's copy (the one the game has).
  `python animations.py pack <game folder> <folder>` writes `animations.pack` there (225 MB, 30 s) and reads every
  entry back; `python animations.py check <folder> [<animations.json>]` reads a pack back and checks it lists what
  the catalog lists, with the same hashes; `python animations.py show <game folder> <name>...` prints entries. The
  pack's format is in the script's doc string: an index of the catalog's names, then each animation's XAnimParts as
  its zone has it (zlib'd), with its bone and notetrack names as strings of its own.
  The reader finds every XAnimParts a zone has (OpenAssetTools' Unlinker lists the same: 3355 in `common_mp`, 1320 in
  `common`, 1026 in `khe_sanh`, 177 in `zombie_theater`), poses of one frame (0 frames) and animations of no bones
  included.

- `setup.ps1 -Game <game folder> [-Out <folder>] [-Pack <folder>] [-Tools <folder>] [-Only <id>...] [-AnimationsOnly]`:
  the user's one-time setup. Downloads OpenAssetTools into `-Tools` (`%LOCALAPPDATA%\IWXMVM\tools` by default) the
  first time, checked against its SHA-256, and builds every character's zone into `-Out`, where the mod looks:
  Plutonium's `storage\t5\iris\zones` by default, `<game folder>\iris\zones` for Steam. 307 zones, 286 MB, about
  two minutes (2026-10-28). The mod finds them as a demo loads (or with Check again on the Look page's Set up...). Then,
  unless `-Only`, the campaign maps' four kits there too (`tools/t5-maps/kits.py`, 30 MB),
 and the animation pack into `-Pack` (the folder `-Out` is in: `storage\t5\iris`); `-AnimationsOnly` writes the
  pack alone.

OpenAssetTools: the release `oat-windows.zip` v0.34.0 of github.com/Laupetin/OpenAssetTools (sha256
`fd2aafd5a6a99d520f67c16ab6e12873da87e8de0b81312d239a1ee2db59d515`), GPL-3. Run scripts that read downloaded or game
files with `python -I`.

The mod loads the zones the scene's actors need, one a frame (`ContentPacks::RunCharacterZoneRequests`), and unloads
those no actor needs, the least recently needed first, once more than 16 are loaded or their textures take more than
512 MB; nothing loads while capturing. A zone is loaded with `ContentPacks::LoadCharacterZone` (harness: `zone <name>`,
`zone unload <name>`, `zone list`, `zone wanted`, `zone wait`, `modelmaterials <model>`, `assetentry <type> <name>`):
everything in it is registered, or shared with an asset already registered by that name, without the two passes a
level pack needs. The game finds a zone in the mod's folder through a hook on the `_snprintf` its loader formats
`zone/Common/<name>.ff` with. Measured on the Nuketown test demo
(2026-10-25): 0.35-0.7 s and 2-12 MB a character, 8-10 MB more once drawn (the textures' larger mips stream in);
unloading one takes 60-230 ms and gives every pool slot back.
