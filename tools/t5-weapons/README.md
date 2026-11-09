# T5 campaign and Zombies weapons

The weapons of the campaign and Zombies that multiplayer hasn't, for actors and the player of a live game.
 Iris Studio's setup does what `weapons.py zones` does in C++ (`SetupLogic::ReadWeaponZones`,
`ReadWeaponTextures`); this script stays the reference and makes the catalog.

- `weapons.py catalog --game <game folder> --oat <OpenAssetTools folder> --work <folder> [--out <json>]` writes
  `core/resources/campaign-weapons.json` (T5iris and Iris Studio embed it; about 3 minutes): for each weapon of its
  lists (the campaign's guns and knives multiplayer hasn't; the Zombies' wonder weapons and World War II guns, each with its
  Pack-a-Punch one; the Pack-a-Punch ones of 28 multiplayer guns) its variants (the attachment `pap` for the upgraded
  one), their display names (the game's string, else the script's), the zone each is in (`t5w_<weapon>`: the variants,
  their left-hand guns and alternate modes), the shot's sound (the first of its sounds a level's bank has, found in the
  linked banks, where an alias name is plain) and the bank's zone (`t5s_<bank>`), whether its shots are projectiles,
  its left-hand gun; the zones with the levels each can be linked from (`sources`, those listed first, then every level
  that has all its weapons); and `textures`, the textures only single player's Rezurrection files have
  (`main/iw_36.iwd` to `iw_41.iwd`, which multiplayer skips: the World at War guns', the Zap Gun's, three of the
  Wunderwaffe's), with the file they go into; and `damage`, what T5iris writes in the defs of the wonder weapons whose damage is
  Zombies' scripts', from the script's `STAND_IN_DAMAGE`.
- `weapons.py zones --game <game folder> --linker <Linker.exe> --work <folder> --out <zones folder> [--iwd <folder>]`
  links the zones with OpenAssetTools' Linker out of the first source level the game has (each level's in one run;
  87 zones, 254 MB, about 40 s), and with `--iwd` writes those textures into `iw_iris_weapons.iwd` there (Plutonium's
  `storage\t5\main`, which the game searches before its own main; 92 textures, 27 MB). `setup.ps1` runs it.

T5iris loads a weapon's zone and its sound zone as an actor holds it (`ContentPacks`, as character zones), as the
player of a live game is given it (`/give <weapon>`), and as a film or game whose weapons name it starts (the weapon
configstrings): `t5/src/WeaponZones.cpp`. Run scripts that read game files with `python -I`.
