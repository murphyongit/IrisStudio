# T5 campaign maps

Tools for loading the campaign levels with `/devmap <level>` in multiplayer as canvases for actors.
 They read the user's own game files with OpenAssetTools (v0.34.0, as
tools/t5-characters) and write nothing into the game folder.

- `poolpatch.py`: writes `t5/src/PoolGrowthTables.hpp`, the tables the mod doubles the image and model pools with
  (`t5/src/PoolGrowth.cpp`): every reference of the renderer's streaming functions (0x70D3D0-0x71A450) to the
  arrays they keep for the pools, and the sizes that go with them, from Plutonium's `t5mp.exe` (133 image and 43 model
  writes, the same as the research's spike), and every instruction of the game that points into the zones' two tables (72
  writes). The mod checks each site against the running game before it writes any.
  Run it again after a game update: `python tools/t5-maps/poolpatch.py [t5mp.exe] [out.hpp]` (needs capstone and
  pefile, which `python -I` hides).
- `worlds.py`: writes `core/resources/campaign-maps.json` (the mod embeds it) and `core/resources/campaign-kits.json`
  (the launcher embeds it). For each of the 18 campaign levels: the assets its world is made of (its gfxworld,
  clipmap, comworld, gameworldsp and map entities, and the models the entities name, linked by the Linker into a
  scratch zone, whose list is the closure: images, materials, technique sets, models, physics presets), what of them
  multiplayer's base zones don't have, its teamset, the fog and vision it starts with (its art
  script read without comments, main()'s first setVolFog with main()'s values, else the first fog of its mission for
  wmd and wmd_sr71), and its
  ambient effects (the oneshots and loops of its createfx, `clientscripts/createfx/*.csc`,
  each by the definition its fxid loads; their definitions are linked with the world, so they are registered with it). For
  each teamset, its kit: the 12 body, head and dog scripts, the 30 player bodies, heads and arms, the RC-XD (both
  looks), the bomb, the HQ crate, the RC-XD's 26 effects and the two faction icons of a multiplayer map of that
  teamset (mp_nuked, mp_havoc, mp_array, mp_cairo first).
  `python -I tools/t5-maps/worlds.py --game <game folder> --oat <folder of Linker.exe> --work <scratch folder>`
  (about 5 minutes; 2026-10-31: the numbers 's table, e.g. cuba 1660 images, 1748 materials, 443 models
  multiplayer doesn't have).
  The 11 Zombies maps are in it too (mode `zombies`): their worlds are linked with
  common_zombie loaded before them, which a map lists in `zones` when its zone refers to it (all but zombietron), and a
  map whose art script sets no vision takes its `level.zombie_visionset`.
  The game folder needs all 11 (a copy without the map packs lacks the 8 DLC maps).
  Each level also gets its row of the game's Change Map menu (`menu`: its place, the language
  keys of its name and description, its picture), the texts of the keys multiplayer's language files lack go in
  `strings` (single player's for Moon and the remakes, ours for Interrogation, SR-71 and Epilogue), and
  campaign-kits.json lists the 25 pictures (`pictures`).
  Each level's doors that open into nothing and stay closed come from `closed-doors.json` (`closedDoors`, below).
- `closed-doors.json`, `closed-doors.py`: the doors that open into nothing: every door of
  a level is left out so it stands open, but behind a few nothing is built. The list is made by looking, each door shot
  from the same spot closed and open (`mapoptions doors closed`, then `mapoptions doors all`); the game's collision can't
  tell, as clips that aren't drawn stand behind them. `python -I tools/t5-maps/closed-doors.py` puts the list into
  campaign-maps.json as worlds.py does, so the catalog needn't be made again.
- `pictures.py`: copies those pictures (the campaign's mission select pictures of frontend.ff and the Zombies map
  select pictures of single player's patch.ff) as DDS files into the mod's `pictures` folder beside its zones, with
  OAT's Unlinker, as setup.ps1 runs it after the kits; the launcher's setup does the same
  (`SetupLogic::ReadMenuPictures`).
  `python -I tools/t5-maps/pictures.py --game <game folder> --unlinker <Unlinker.exe> --work <folder> --out <folder>`
- `kits.py`: builds the four kit zones (`t5m_kit_<teamset>.ff`, 7.2-7.9 MB each) into the mod's zones folder, as
  `tools/t5-characters/setup.ps1` runs it after the character zones; the launcher builds the same with the character
  zones (`SetupLogic::ReadKitZones`).
  `python -I tools/t5-maps/kits.py --game <game folder> --linker <Linker.exe> --work <folder> --out <zones folder>`

The mod (`t5/src/CampaignMaps.cpp`) registers a level's world alone from its zone, under multiplayer's types and names,
with its entities rewritten (`CampaignMapLogic`) and stand-in scripts, and loads the kit of its teamset with it. The
harness checks them with `campaignmap` and the scenarios `campaign-maps` and `campaign-film`.
