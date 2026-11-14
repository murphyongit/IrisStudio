# S1iris working notes (addresses are s1_mp64_ship.exe, image base 0x140000000)

Build/install loop: commit + push `fork main` -> Actions `s1.yml` -> release `s1iris-latest` (gh api REST only, no GraphQL) ->
curl to /mnt/user-data/outputs/s1iris/s1iris_vNN_XInput1_3.dll -> device_commit_files to D:\GAMES\AW\XInput1_3.dll (force) ->
device_stage_files + cmp. Game must be closed (file locked otherwise). Log: D:\GAMES\AW\theater\s1iris.log. Version string in theater.cpp.
RE tools: /home/claude/awtheater/tools (s1dis.py <exe> <addr> [n] | callers <addr>; s1scan.py <exe> strings|xrefs <regex>; awx.py find|calls|callers <regex>
for AW Xbox names), exe /home/claude/awtheater/test/s1_mp_unpacked.exe, run with python3 -I. /tmp/fl.py lo hi lists functions (may be gone).

## Found so far
- centity array 0x141655F60, 0x288 each, number word +0x144; Com_GetClientDObj(handle, client) 0x1403D2BA0 (map 0x147DA8660, objs 0x147B76660, 0x190 each)
- SL_FindString 0x1403F11C0; CG_DObjGetWorldTagMatrix(cent, dobj, tag, axis[3][3], origin) 0x140190D00 (local tag matrix 0x140190A70)
- Com_ClientDObjCreate(models, count(u16), tree, handle) 0x1403D2AF0; DObjModel entry 0x20 bytes: model ptr, tag(scr string) +8, flags +0xC,
  +0x10 override material ptr (or 0), +0x18 the model's own material ptr. Handle 2048 = first person arms+gun. DObjCreate 0x1404DC8B0.
- CG_PlayExoFx(client, ent, event, parm, firstPerson, ...) 0x14019A230 (hooked: firstPerson=0 when camera is outside)
- R_AddOmniLightToScene(origin, dir, radius, r, g, b, bulb[4]) 0x1405C9D90; scene light array 0x14F8E8038 (0x98 each, count 0x14F8E8030, max 32);
  game's own bulb block: {1, 0, 0, -1}, colours in the hundreds. Spot light function: chunk at 0x1405CA071 (start near 0x1405C9FE0), 11 args, NOT decoded.
- FX_PlayOrientedEffect(client, def, cgTime, origin, axis) 0x1402AC700; FX_Register(name) 0x1402A0E50; cg time int 0x141557580
- DB_EnumXAssets_Internal(type, cb, data, false) 0x14026EC80; types: xmodel 7, material 8, image 16, gfxworld 31, fx 42, stringtable 51
- GfxWorld: skyCount +0x20, skies +0x28 (56 each, image +0x10); GfxImage: views at +8, +0x10
- Cmd_AddCommandInternal(name, fn, storage) 0x1403AF2C0; cmd_args 0x1479ECB00 (nesting, argc at +0x44, argv at +0x68)
- (WRONG until 0.61, see "0.61" below) 0x140026A20(name) is the transient UNLOAD (count -1), not the load; 0x1400268D0(index) takes one count off;
  0x140025470 finds a name's indices. Calling 0x140026A20 as a "load" took models from under the players -> crash (0.44).
  Proper fix = change the player's customization so the game streams it itself.
- refdef 0x1415575C0 (tanX +0x10, origin +0x18, axis +0x24), view origin copy 0x141557610; CG_CalcViewValues hook 0x1401B7BB0; scene handover 0x1401DF410;
  CalcViewValues body 0x1401DC450; FOV getter 0x1401D59D0; R_UpdateFrameSun 0x1405D3AF0; sun override block 0x14D859530
- Tables dumped to theater\tables: mp_charactergearsettable (set -> item ids 0x6CVVPPP: C category digit, VV skin variant, PPP primary id),
  mp_costumeidtable (cat, idIndex, ?, ?, primary, loc, image, id), mp_costumemodeltable (model, cat, idIndex, primary, collision, transient,
  lobmodel, viewmodel, worldmat, viewmat, group1 female/!female), mp_costumeoverridetable (model, name, guid, overridemat, overrideviewmat), mp_camotable, mp_camomaterialtable.

## Open (user's order): proper gear fix via customization, camos, ray/trace function (right-click placement), flashbang/shellshock in free cam
(playerstate fields), spot lights. Also: rain from mp_refraction on other maps (effects only exist in that map's files).

## Added in 0.46 (untested when written)
- R_AddSpotLightToScene 0x1405C9FE0(origin, dir, up, radiusScale, innerScale, outerScale, r, g, b, lightDef (default at 0x14D85F500), SpotBlock*);
  SpotBlock = FxSpotLightDef {halfFovOuter, halfFovInner, radius, brightness, maxLength, int exponent, nearClip, bulbRadius, bulbLength, fade[2], u8 shadows}.
  Max 2 per scene (count 0x14F8E7EF0, array 0x14F8E7EF8).
- Gear streaming: CL stream sync handler 0x140022290(client, lists, flag): lists = 16-byte entries per customization kind {u16 count, u32* numbers @+8},
  kinds 0..9 are costume kinds; per-kind stored copy at 0x140BB1C10 + kind*16. Model name of (kind, number): 0x14000C100(0x1403CC830(kind), number).
  models.cpp adds a set's (kind, number)s to the lists as they arrive (g_extra), then rebuilds; DObj swap applies once loaded.
- CG_PlayExoFx etc. as above. Still open: camos, trace function, shellshock fields.

## Rain (0.47)
- x64 ZoneTool (github.com/Joelrau/x64-zt, zonetool.exe in D:\GAMES\AW, user installed it) + D:\GAMES\AW\s1iris_rain_build.bat (pipes "dumpmap mp_refraction"
  after 40 s, moves dump\mp_refraction to zonetool\s1iris_rain, -buildzone s1iris_rain; csv zone_source\s1iris_rain.csv = 3 require lines + 37 fx).
  Built OK: D:\GAMES\AW\zone\s1iris_rain.ff (43 KB, all 37 fx). The bat renames XInput1_3.dll while the tool runs.
- effects.cpp loads it with DB_LoadXAssets(0x140270F30) {name, allocFlags 1 (common), 0}, async, when asked from the Effects panel. Untested when written.
- Gear transient files are zone\mp_<kind>_..._tr.ff (e.g. mp_hat_vl_583_tr.ff). .bat files cannot be sent in chat; device_commit_files of exe was denied,
  csv/bat updates were allowed afterwards. The shell rewrites ">nul" in commands: build such text in python.
- 0.47 called DB_LoadXAssets from the calc-view hook: the main thread never came back (user force-closed). The function first waits for the database
  thread (Sys_SyncDatabase 0x1403E2170, event at 0x147DCEC28; Sys_IsDatabaseReady 0x1403E1850) and later takes a write lock (0x143B749C8/CC).
  0.48 sends S1x's own console command "loadzone s1iris_rain" through Cbuf instead (S1x has loadzone: console_commands.txt), once the loader is idle.
- 0.48 loadzone got as far as "MISSING FILE eng_s1iris_rain.ff": for an ordinary zone the game also queues patch_<name>, <lang>_patch_<name> and
  <lang>_<name> (0x140271B09..); only the patch_ ones are optional (0x1402701F0), as are names with _load/_path or starting techsets_.
  A zone whose own name starts with patch_ gets no extras. Fix used: zone\eng_s1iris_rain.ff = a copy of s1iris_rain.ff (written to the PC).
  Correction: language zones are read from zone\english\ (eng_*.ff), not zone\; the copy goes there (a stray copy is also in zone\, harmless).
- Next error: "Mixed compression types are not supported" (ZoneTool writes zlib, type 1 at header byte 13; stock is LZ4, type 4). Check at 0x140242DF0
  (first kind kept in 0x141F9AF74, flag 0x141F9AF71; g_compressor 0x141E0B080). 0.49 does what s1-mod's fastfiles.cpp does: nop 0x140242DF7 (2 bytes),
  hook DB_ReadStreamFile 0x14027AA70 to set g_compressor 4, nop 0x1402427A5 (unsigned files). LZ4 body framing, if ever needed: u64 total size,
  u32 (block size 0x10000 : 24, type 4 : 8), then per block u32 packed size, u32 unpacked size, raw LZ4, padded to 4 (stock eng_ui.ff is one such block).

## 0.50: right-click placement, flashbang
- Line check: G_TraceCapsule 0x1402F93C0(trace_t*, start, end, Bounds*, u16 passEnt (0x7FF none), int mask) -> SV_SetupIgnoreEntParams 0x1404579A0, SV_Trace 0x140458140.
  Found through the script builtin physicstrace (0x140310A40, mask 0x820011); bullettrace is 0x1403102B0 -> G_LocationalTrace 0x1402F7520, masks 0x280E831 / 0x80A831
  (no characters). trace_t starts with fraction, then (assumed) normal[3]. Script builtin tables: 0x1409B5DE0 (0x163), 0x1409B7460 (0xBA), 0x1409B8660 (0x11),
  0x1407FB260 (8), 16 bytes each {func, u16 name id, s16 type}; Scr_GetVector 0x1403F8EC0, Scr_GetNumParam 0x1403F8980.
- camera.cpp pick_now() runs it in the render-scene hook from REFDEF (origin, axis, tan x/y) for the point of the picture clicked; ui.cpp "place" popup.
- Flashbang/stun screen: CG_DrawShellshockBlend(client) 0x1401D5420 (called from R_RenderScene 0x1405CE650; blend time 0x1401C0020, flashed 0x1401BFE70,
  blurred 0x1401BFDB0). Hooked: skipped while the camera is away from the player.
- Still open: camos.

## 0.51: camos, resizable panels, timeline zoom
- Camo (painted ones, camo table rows with a column index into mp_camomaterialtable: reptile..blops, 16 per weapon): the game sets the weapon model's
  DObj list entry: override material at +0x10 = m/<weaponprefix>_<camo>, byte +0xD = 1 (seen: [vm_dear_base_loot 00002A26 00000100 m/dear_jackpot]).
  models.cpp g_camos does the same in the Com_ClientDObjCreate hook; only materials already loaded are offered (log line says how many are).
  Older camos (table rows 1..~19, an image like camo_multicam_e_col and flag 1) work some other way: not found.
- ui.cpp: panel width / timeline height dragged on the lines between (double click resets); timeline zoom (wheel, Shift+wheel pans, Fit).

## 0.52: materials really put on; players remade directly
- DObjCreate (0x1404DC8B0, from Com_ClientDObjCreate 0x1403D2AF0) reads only model/tag/flag byte +0xC of each entry; a model with word +0x204 & 0x400 is
  several models (count byte +0x202, array +0x298). The entry's materials are applied by a SECOND call with the same list:
  0x1404DE730(obj, list, count): entries with +0x10 and +0x18 both set; byte +0xD != 0 -> 0x1404DEB70(obj, model-or-part, from=+0x18, to=+0x10)
  (material replaced); byte 0 -> decal maps copied. Game wrapper 0x1401A2BF0(list, count, tree, handle, client, handle) = create + 0x141799F60 cache reset +
  overrides + 0x1404DEA20(client, obj). First-person builder 0x1401EDDF0: weapon entry +0x18 = 0x1401A2CD0(model, weapondef material @+0x40 via 0x140169C20),
  +0x10 = camo material 0x140341420(client, camo-1, &byteD). In the user's recording the AE4's entry had +0x10 = m/dear_jackpot but +0x18 = 0 (!).
- 0.52 hooks 0x1404DE730 to hand it the editor's list (thread-local, keyed by the obj just made), fills +0x18 for camo with a guessed base material
  (model_materials via 0x1404F8070/0x1404F8050; logged per model), and remakes a player's DObj itself when choices change: Com_GetClientDObj 0x1403D2BA0,
  tree = *(obj), Com_SafeClientDObjFree(handle) 0x1403D2DF0, then 0x1401A2BF0 with the game's last list (rewinding did not always make the game rebuild).
  First person (2048) still rewinds. Untested when written.
- Timeline: names column draggable, trash per effect row.

## 0.53: gear sets matched the game's way
- mp_costumemodeltable cols 10..17 are conditions on TAGS ("female", "!female", "kneepads", "!shinguard", a head's name, "!fullmask", "helmet", "eyewear",
  "skinN"); mp_costumeidtable cols 9..11 are the tags an item gives (head: name, skinN, female; kneepads item: kneepads|shinguard; hat: helmet|fullmask, visor;
  eyewear: eyewear). Categories by id prefix: gender 61, shirt 62, head 63, pants 64, hat 66, gear 67, kneepads 68, gloves 69, shoes 6A, exo 6B, eyewear 6C
  (set columns in that order: eyewear, hat, shirt, gear, gloves, pants, kneepads, shoes, exo). Skin = bits 12..19 of the id; override rows are per model.
  models_apply_set now gathers tags (player's head + what the set gives + current gear of kinds the set lacks) and picks the row whose conditions hold;
  a full mask drops the head, a helmet on a masked player adds a head. Checked offline: "samurai" resolves to exactly the user's own recorded outfit.
- Old code read only col 10 as gender, so pants (gender in col 12) and hats (head name) got an arbitrary row.
- Loading of parts not in memory is still the 0.46 stream-sync path behind the checkbox, never yet run by the user.

## 0.54: loading a set's missing parts, for real
- The gear table in memory: entries 0x58 bytes at 0x140B33B90 {+0 model name, +8 lob name, +0x10 name loaded by, +0x18 item word, +0x1C.. tag masks};
  per kind {u16 first, u16 count} at 0x140B33B48 + t*4 (t = 0x1403CC830(kind) = kind+1); flag 0x140B5F0E8 "in use".
  0x14000C100(t, n) = entry[first-1+n] (n from 1); 0x14000C3D0(g) = entry[g-1]+0x10 (g = place in the whole table, from 1) and that is what the sync
  handler 0x140022290 loads with CL_LoadTransientByXModel 0x140026A20. So list numbers are whole-table places. 0.46-0.53 asked for number 0: found nothing
  ("0 gear models have a number"), so the checkbox never did anything.
- 0.54: read_numbers reads the table directly; the last lists the game was given are kept (deep copy) and the game's handler is called again with the
  set's missing parts added (replay_sync), falling back to rewinding if no lists were seen. Untested when written.
- Weapons: npc_/vm_ "_loot" models are whole guns (composite: word +0x204 & 0x400, parts byte +0x202); "_base" ones are bare receivers. Picker group 4.
- 0.54 crashed on Apply (FAULT 0x140025470 reading 0, from 0x1400256E0(name=NULL) <- stream update 0x140023B4A): for kinds 0..9 the lists hold the number
  WITHIN the kind (0x1400237F0: name = 0x14000C100(0x1403CC830(kind), number & 0xFFFF)), not the place in the whole table (that is list 11 only).
  Seen layout (first+count per t): 0+0 1+76 129+38 193+146 449+24 481+21 513+25 545+84 673+619 1697+204 1953+34 (shirt, head, pants, gloves, shoes,
  kneepads, gear, hat, eyewear, exo). 0.55 uses i+1 and checks each number with the game's own naming before adding it. The handler replay itself returned fine.

## 0.56: loading works; memory budget; random outfits
- 0.55 loading confirmed by the user ("it worked"): parts arrive within about a second of the handler replay. With 17 extra gear models on top of the
  match's own 103 list numbers it was fine; asked for 21 the game ran out of memory (no error reached the log). EXTRA_MOST = 17.
- g_extra used to only grow. Now release_unused(): 1.5 s after a change, once every changed player's DObj has been remade, extras no swap/add refers to are
  taken off the lists and the handler replayed (the game unloads them); g_loaded is re-read then and 3 s later.
- models_randomize(mask): per entity a random set among those whose not-yet-loaded parts fit the remaining budget (build_set + set_cost); sets queue in
  g_pending and are applied one after another. Loading is on by default now.

## 0.57: what the gear memory limit really is
- 0.56's release did nothing: after 7 numbers were taken off the lists the loaded-model count never fell (3979 -> 3982 -> 3984), and the next loads
  killed the game with no minidump and nothing in the log: the WIN_OUT_OF_MEM box (0x1404D6C20, code 0x1A) from the transient allocator 0x1400247E0.
  That allocator places a transient file in ITS SLOT: pools are 0x78-byte descriptors at 0x140BB79D0 (slot size +0x40, base +0x58, per-slot sizes +0x70),
  slot cursors 16 bytes each at 0x140BB7FF0; a file's registry entry (0x140BB8F10, 0x30 each, MAX_TRANSIENT_FILES 2700) has pool byte +8 and slot word +6.
  When cursor + size passes the slot's size -> out of memory. So the limit is slots, not a total; pools are sized at map load (function 0x140272C53..,
  "Exceeded transient resident memory budget when allocating pools"). Counts seen fine: 17 and 19 extra gear models; died asking past that.
- 0.57: no release; room = models ever asked for that are loaded now (g_asked_models) + planned, at most 17.
- To really raise it: (a) make the game unload gear taken off its lists (find what its handler needs: flags 0xC0000000 on list numbers? 0x1400259A0?),
  and take a re-dressed player's own gear off the lists too; or (b) more/larger slots at map load. Neither done.

## 0.58: HUD, effects list, timeline rows, bone camera points
- HUD parts (LUI names): kill feed wrapper_for_obituary_rotation_id, +100 wrapper_for_pointsPopup, hitmarkers wrapper_for_damageFeedbackHud, weapon/streak
  info wrapper_for_baseWeaponInfoHud, hints wrapper_for_hintsContainerHudId, messages wrapper_for_gameMessageHudId, medals wrapper_for_splashesHud.
  "Kills only" hides every third-level part but the first three. show() no longer prints on the game's screen during playback (it landed in the kill feed).
- Effects list: one line each, details on click, order combo, Copy + Paste (list button and the picture's right-click menu).
- Timeline: Ctrl+wheel = row height (g_lane_scale). Orbit camera removed from the list.
- Bone camera: tracks bone_offset, bone_turn, camera_fov (applied only as time moves); "Add a camera point here" keys all three.
- Fog: only r_fog, r_fog_ev_adjust, r_sky_fog_* are settable; the g_*Fog*ReadOnly dvars are outputs. Real fog colour/density = the cg fog state set by
  script (setExpFog / atmos fog): not found yet. Subdivision/tessellation dvars do nothing on MP models (rows removed).
- Still open, asked for: unload gear (option 1), render passes (depth, green screen, world, weapon), gun vs ReShade depth, fog colour/density.

## 0.59: gear let go of with the game's own call (option 1, first half)
- Handler 0x140022290: lists 10/11/12 (weapons, gear by whole-table place, viewmodels) are diffed and loaded/unloaded at once; per-kind lists 0..9 go through
  0x140023460(client, kind, ...): 0x1400233D0 marks stored entries still present (bits 0xC0000000), 0x1400236D0 loads new ones, 0x140023510 unloads the rest:
  names via 0x1400237F0, sets [0x140BB1C10 + client*0x5DC0 + 0x5DB4] = 1, calls 0x1400259A0(name) (-> 0x140025470 finds the files, 0x1400257D0 releases).
  Part of that is gated by [0x140BB79C0]; in a match the replayed handler loaded nothing itself (the periodic update 0x140023B4A did) and unloaded nothing.
- 0.59 release_unworn(): 2 s after a change, once changed players are remade: asked-for models no swap/add uses are taken off g_extra (handler replayed),
  0.5 s later 0x1400259A0(name) is called for each; 2.5 s later the log says how many asked-for models are still in memory. Untested when written.
  If it works: next, drop a re-dressed player's ORIGINAL gear too (so every player can differ).
- Also 0.59: camera mode loop used a fixed 6 (garbage button after Orbit was removed); bone fly speed of its own (g_bone_speed, 40); effect clips fire when
  the settled time is inside the clip (g_inside), not only on crossing its start.
- Not done, asked for: HUD timers follow real time (kill feed stays after a rewind, fades too fast in slow motion) -> needs the LUI frame clock.

## 0.60: kill feed and popups cleared on a rewind (HUD script)
- lua/s1iris/__init__.lua (installed by hand to D:\GAMES\AW\s1x\ui_scripts\s1iris\__init__.lua; S1x loads it at game start): flush() watches what is under
  the obituary, pointsPopup, splashesHud and killstreakEarned parts; after 10 s with no change it takes what is there as the HUD's own; when the dvar
  s1iris_hud_flush changes (theater.cpp go_back_to counts it up) it closes elements not among those whose parent is. Untested when written.
- HUD timers/animations run on real time in the engine (LUI). Looked for the clock: 0x1405B01B0 only sets the renderer's time (cg time in a match);
  0x1400CE810 is a Lua C function (animate), 0x1400D0040 dispatches the animation-complete event ("lateness"); LUI_CoD_Layout computes dt inside (not found
  on PC). So fades still ignore the playback speed.
- User's roadmap after this: death animations like T5 Iris, then actors (spawned fake players with animation chains). Spot lights exist since 0.46
  (Scene > Lights > Kind), never confirmed by the user.

## 0.61: the transient system read right; real unloading; exact room; presets by section
- 0.59's release crashed after ~4 sets (out of memory, nothing in the log): 0x1400259A0(name) is the LOAD (count +1; 0x1400257D0 per index: state 0 -> 1 and
  queued) and 0x140026A20(name) is the UNLOAD (0x1400268D0 per index: count -1; file at 0 -> state 6, unloaded >= 2 frames later by 0x1400263A0 with
  DB_LoadXAssets flag 0x40, then 0x140272960 and the slot handed back by 0x140024970). I had them swapped since 0.44.
- Asset index table 0x140BDA050, 12 bytes: +0 hash, +4 next, +6 file number, +8 count. File registry 0x140BB8F10, 0x30 bytes: +6 slot (0xFFFF none),
  +8 pool, +9 name, +0x2C state (0 unloaded, 1 queued, 2 loading, 3 loading but unwanted, 4 loaded, 6 unload queued, 5 unloading), +0x2D count.
  Work queue 0x140BEE210 (count 0x140BEBA28); frame counter 0x14D858A04; pump 0x1400259E0.
- Handler 0x140022290(client, lists, flag): flag 0 = lists BEGIN: 0x140022000 forgets the stored lists (0x1403CC7E0 zeroes 13 counts, nothing unloaded)
  and everything on the lists is loaded (+1) again; lists 10 (weapons) and 12 are skipped. flag != 0 = lists CHANGED: stored vs new diffed for kinds 0..9,
  11, and 10/12: gone -> 0x140026A20 + 0x1401EECE0(name, kind==10) (frees every DObj that uses the model: the 42 character entries at 0x141620568,
  0xCC0 each, and with the flag the snapshot's entities), new -> 0x1400259A0. All gated by "active" [0x140BB1C10 + 0x5DB0] (= 0x140BB79C0).
  The editor replayed with the server's flag (0): +1 on all 103 + extras each time, nothing ever unloaded. The demo's own syncs on every rewind are flag 0
  too, so counts grow by one per rewind (bytes).
- Lists: {u16 count, u16 capacity, ptr +8}; stored capacity 18 for kinds 0..9, 52 for list 10; the handler's temporary list holds 63 (no bounds check).
- Pools (0x140024464): one per gear kind, "mp_char_shirt/head/pants/gloves/shoes/kneepads/gear/hat/eyewear/exo", each min(18, models of the kind) slots
  (18 = default, or [0x1403D25E0()+0x74]); then mp_worldweap, mp_viewarm (6), mp_viewweap (4). Pool 0x78 bytes: +0 name, +0x40 slot size, +0x48 slots,
  +0x50 tag, +0x58 memory, +0x60 first slot entry, +0x68 free-list head, +0x70 per-slot sizes (then no free list). Slot entries 16 bytes at 0x140BB7FF0
  (+0 cursor, +8 pool, +0xA next free), 242 at most (count 0x140BB7FEC): no room to enlarge the pools. Taking a slot (0x1400248E0) does not check for
  0xFFFF: a 19th file of a kind = WIN_OUT_OF_MEM. So the limit is 18 PER KIND, not ~17 overall.
- 0.61: replay_sync replays as a change (flag 1, lists 10/12 = the game's stored ones); release_unworn takes unworn asked-for gear off the lists (handler
  unloads), then 0.5 s later tr_let_go() takes off whatever counts are left (from rewinds) and calls 0x1401EECE0; room is counted per pool from the free
  lists minus files in states 1..3 (read_room, add_need/set_need); EXTRA_MOST is gone. Lists capped at 18 per kind. Untested when written.
- Not done (second half of "option 1"): dropping a re-dressed player's ORIGINAL gear. The game's clientinfo (and corpses' copies) keep XModel pointers to
  the originals, so unloading them leaves stale pointers that the game hands to DObj creation. The clean way is to change what the player wears at the
  costume level (hook the costume reader 0x14000C0D0(kind, customization) / the clientinfo models) so the game resolves, streams and drops by itself.
- Presets: sections (HUD, Sun, Sky, each Look section, Other settings); a "hud_hide" line holds the HUD's hidden parts; load_preset(name, only).
- The HUD's clock, found: LUI is stepped by 0x1400D7480(client, root, dtB, dtA, flag, lua) which sends the Lua step event with "deltaTime" (= dtA, or dtB
  for a root whose byte +0x98 is set). Its caller 0x1400E5790 takes dtA from Sys_Milliseconds (0x1400E585B) against the root block's +0xA8 (block from
  0x1400E4A60(root)); with its flag 0 both deltas are 0. Frame entry 0x1400E6810(client, flag) -> 0x1400E5690 -> 0x1400E5790; drawing is 0x1400E61C0 ->
  0x1400DA6D0. 0.61 hooks 0x1400D7480: in playback dtA = the recording's clock (g_clock) since that root's last step (0 backwards, 60 s at most), unless
  a menu has the keys (key catchers 0x1417E168C other than bit 1) or the editor just told the HUD's script something (theater_hud_wake, 0.5 s real).
  The script's own 100 ms tick therefore stands still while paused: "quiet for 100 ticks" is now 10 s of the recording's time. Untested when written.

## 0.62: passes, the gun's depth, the fog's own values (all untested when written)
- 0.61 confirmed by the user: gear release works ("no crashes, outfits basically solved"; log: free places by kind move up and down), HUD clock hooked
  ("root 141432860 follows the recording's clock (key catchers 0)").
- rgp.world = [0x14D85F640] (GfxWorld; dpvs at +1808 = 0x710: smodelCount +0x710, surface ranges +0x71C..+0x748, smodelVisData[0] +0x758; layout in
  x64-zt src/zonetool/zonetool/s1/structs.hpp).
- 0x1405CB8D0(a, b, c): queues the map's BSP surface ranges for the camera (calls 0x14056A0F0(begin, end, args) five times; only caller of it);
  0x1405C9AE0(a): the static models' worker command 0x1B (R_AddWorkerCmd = 0x1405E5130); both called from the scene builders 0x1405CBD30/0x1405CBF70.
  Skipping both = the map and sky not drawn (passes.cpp hide_world). Scene models (dropped weapons etc.) and brush models are still drawn.
- 0x1405C8BB0(a, b, sceneEnt, d, e, f, g, h): one scene DObj's camera surfaces (caller loop at 0x140579D98, entries 0xB0 bytes: DObj +0x68, flags +0x94,
  bits 1..3 looked at as "(flags >> 1) - 1 & 7" and "flags & 0xE": taken to be the depth hack). Returns at once for a null DObj, so skipping is safe.
  Shadows are a different path (a hidden player still casts one).
- Com_GetClientDObj 0x1403D2BA0: word map at 0x147DA8660[handle] -> DObj 0x147B76660 + index*0x190 (second argument unused).
- No dvars for hiding world/entities or showing depth exist (checked dvars.txt): r_clear (3 = steady) + r_clearColor do the colour behind.
- Depth: found by hooking ClearDepthStencilView (vtable slot 53) on the immediate context and on a deferred context's vtable; candidates = depth
  textures of the back buffer's size; copied at present (or before the n-th clear) into a typeless SRV texture and drawn by a full-screen triangle
  before ui.cpp's targets() copies the back buffer. The log lists the candidates ("passes: depth buffers ...") the first few times.
- Fog: per client 7 sets of 0x9C bytes at 0x14D858C34 (set 2 = drawn/current, 3 = blended from, [0x14D8594BC] = the set being gone to; the server's is 1).
  Setter 0x14059EC50(client, set*, index); blend 0x14059ED00(client, index, start, duration); the "atmosfog"/fog command handler 0x1401BD580 gives the
  layout: +0x3C atmos flag; plain: start +0x00, half +0x80, colour +0x7C, sun colour +0x84 (if +0x08), +0x24/+0x04/+0x0C.. more; atmos: first two
  floats -> +0x50, +0x8C, colours +0x94 (sun fog) and +0x98 (haze), then +0x40, +0x44, +0x4C, +0x48, +0x54, +0x58, +0x5C, +0x60, direction
  +0x64..+0x6C, +0x70, +0x90. Colours are 4 bytes B,G,R,A (packer 0x1401BA430). passes.cpp writes wishes into the going-to set and set 2 each frame.
  The dump function with the names (ent.atmosFog...) is 0x1401CDD09: reading it would name the atmos numbers.
- Multi-pass recording: capture.cpp runs the ticked passes one after another over the same stretch (finish() -> next pass -> seek), file names get
  the pass's word (passes_tag).
- Not done: older pattern camos; dropping original gear (see 0.61); scene/brush models in the colour passes.

## 0.63: original gear dropped (by name in the character records); 0.62's passes and fog fixed (untested when written)
- User on 0.62: Everything / no gun worked; "only the world" kept the players; the colour passes hid players and gun but drew the map; fog did nothing;
  "gun at its true depth" worked (in effect only r_znear 0.5: the flag stripping never ran, see next line; kept as r_znear only).
- passes: the scene entry's DObj is at +0x98 (the argument of DObjGetNumModels in 0x1405C8BB0), not +0x68 (0.62 matched nothing: "0 held back").
  The map is drawn by worker commands (dispatcher 0x1405E5B80, jump table at +0x5E5EB0, 0x24 types): 0x10 -> 0x140569EB0 (BSP, usual path; uses the
  second world pointer [0x14D8F2640]), 0x12 -> 0x14056A2F0 (BSP ranges), 0x1D -> 0x14056FFD0 and 0x13 -> 0x14056D940 (static models), 0x11 ->
  0x14056B110 (unknown, big stack, queued with the FX command 0x19), 0x15 -> 0x140579B90 (scene DObjs), 0x16 -> 0x1405CBD30, 0x1B -> 0x1402A5410 (FX).
  [0x14D80FD98] != 0 selects the range path (0x1405CB8D0 queues all five ranges); normally 0 (only the emissive range goes that way).
- fog: per frame 0x1405D347A writes set 2 (drawn) from set 4 (the "to" copy; times at +0x74/+0x78 of it) or a blend from set 3: 0.62 wrote sets
  [going_to] and 2 only, so it was overwritten. 0.63 writes all seven; the log line "fog: ..." gives the game's own values and what was asked.
- depth: no ClearDepthStencilView of a 2560x1440 buffer was seen; 0.63 also watches OMSetRenderTargets (slot 33), takes any size, picks by score, and
  logs every candidate with its size.
- Character records: 0xCC0 bytes: +0 handle, +4 models-loaded flag (cleared by the per-frame check 0x1401B06E0 via 0x1400239F0(client, name) ->
  0x1400256E0), +0x24 first model NAME, +0x64 + i*0x40 twenty more names. Arrays: 42 players at 0x141620568, 8 at 0x141649350 (entity - 42), bodies at
  0x14164F950 (entity - 50). models.cpp rename_gear(): a swap whose two names are both gear is written into the player's record (g_renamed,
  g_unrenamed maps it back for hooked_create so g_seen/g_swaps stay in terms of the player's own gear), the DObj freed for the game to rebuild; the
  displaced own gear is then dropped (g_dropped: filtered out of the server's lists in the hook and the replay, counted off with tr_let_go) when no
  record names it and no live DObj's list holds its pointer. Going back: the number is un-dropped, reloaded, then the name written back.
  If the game rewrote the records every frame the pass turns itself off (g_names_off, "keeps writing the players' records back").
  Unknown: whether DObjs are built from the names each time (then originals really leave) or from cached pointers (then nothing is dropped, and
  nothing breaks: the log will show no "let go" of own gear).

## 0.64 (untested when written)
- User on 0.63: depth showed a wrong, smaller buffer (looks like the sun shadow map); the gun-only colour pass still drew many props; fog did nothing;
  wants player effects gone with the players; wants a reset on every setting. Log: no depth buffer seen through slots 33/53 at the logged frames;
  every fog set EMPTY (going to set 0; all zero) on that recording (mp map with Ascend's rain loaded) -> there was no game fog to change;
  "the game keeps writing the players' records back (420 writes in 3 s)" -> the records are rewritten a few times a second from the match's data.
- Gear by name now lives with that: a record's name gone back to the original is rewritten silently (no DObj free); the DObj is freed only when what is
  wanted changes. Guard counts frees (300 in 3 s) and, when it trips, un-drops everything. Free places after 0.63's short run had risen (shirt 6,
  pants 4), so own gear does leave memory when renamed.
- Passes: command 0x11 (0x14056B110) held back too; FX_PlayBoltedEffect 0x1402AC420(client, def, time, dobjHandle, bone, ...) held back for hidden
  players' handles (all handles below 2048 in the gun-only pass). Family of FX play functions 0x1402AC45F..0x1402ACCC0 all end in 0x1402B2490 +
  0x1402AC050; bolt lookup 0x1402B1400(client, handle, bone, out).
- Fog: when the game's sets are empty a plain fog can be made (start +0x00, half +0x80, colour +0x7C, +0x04 taken as max opacity, +0x24 as HDR colour
  intensity, +0x14 scale): guesses from the classic argument order.
- Depth: also hooks OMSetRenderTargetsAndUnorderedAccessViews (slot 34) and depth-only binds; by itself only a buffer of the picture's size is shown.
- ui_reset_menu(id): right-click "Reset to default" on a row (every Look setting through control(), the Passes rows).

## 0.65 (untested when written)
- User on 0.64: reset works; depth shows nothing; colour passes better but loose items and effects still show; made fog works by distance but is black;
  random outfits work (log: no "over and over", free places up to shirt 9 / hat 8: own gear is leaving memory).
- Depth: NOTHING ever came through the hooked context methods (slots 33/34/53, immediate + a deferred context's vtable) though the game calls them by
  vtable: clear at 0x1405D95E4 in 0x1405D9510(holder, which, colour, depth, stencil) (context = *holder; it OMGetRenderTargets (slot 89) then clears),
  also 0x1405FAFCB; OMSetRenderTargets at 0x1405D4B2F/0x1405D4B3F (fn 0x1405D4A70), 0x1405DB6F2, 0x1405DBFF1. So the game's contexts have another
  vtable than the two hooked. 0.65 hooks 0x1405D9510 itself and asks the context for its DSV.
- cmd 0x15 handler 0x140579B90: scene DObjs (0xB0 each) -> 0x1405C8BB0; scene models (0x88 each) and a second list -> 0x1405CA7A0 (10 args);
  brush models (0x50 each) -> 0x1405C7FD0 (8 args). Both held back with the map.
- FX: every play function gets the system from 0x1402B2490(client) and spawns through 0x1402AC050 (26 args: def at stack +0x40, time +0x48, origin
  +0x50, axis +0x58, dobj handle +0x60 (0xFFF none), bone +0x68 (0x7FF none)); effects are 0xB0 bytes (def +0, time +0x40). hooked_spawn holds back
  by what the effect is fixed to; choosing a pass by hand calls theater_rebuild() so running effects begin again under the pass's rules.
- Made fog: +0x24 now a "Brightness" slider (1 gave black on a night map).

## 0.66 (untested when written)
- User on 0.65: "As depth" WORKS (found through the game's own clear 0x1405D9510). Colour passes better; still drew rain, puddles, some lights, glass
  panes, smoke; gun-only drew a shot player for a moment; only-the-world drew bodies; made fog had colour but was inside out.
- FOG, solved: the frame's fog (set 6, +0x3A8, copied each frame to [0x14D932680] + 0x540FBC) is read by 0x1405FB553: shader gets 0, 1 - (+0x04)
  (so +0x04 IS max opacity), -(+0x80), (+0x80) x (+0x00), colour bytes/255 linearised (0x140588E70) x 0x14059EBF0(+0x24, exposure); sun: angles
  +0x0C/+0x10 (degrees), direction +0x18, colour +0x84, intensity +0x28, scale +0x14. So +0x80 is the DENSITY (ln2 / half-way distance), not the
  distance: writing the distance gave exp(+huge) near the camera. Set 5 (+0x30C, times +0x380/+0x384) is a second fog blended over the current one.
- 0x1405C8840(entry, n, out, end, e) -> new out: the second DObj surface lister (callers in 0x1405A91F0, reached from 0x1405CBBC0): same rule applied.
- On a colour: fx_draw 0, fx_flare 0, r_drawWater 0 unless "Keep effects" (then the spawn filter of 0.65). Glass panes are not handled yet.
- Characters for the passes: 42 player records + 8 + 24 body records (handle from each).

## 0.67

- **Why a shot player flashed in the gun-only pass:** the table of models by handle (0x147DA8660) has 2050 words, handles
  0..2049. The dword right after it (0x147DA9664) is the place of the model made last (where the search for a free
  place starts, 0x1403D2BF0). passes.cpp asked for handles 2048..2051, so "handle 2050" was always the newest model:
  a body just made counted as the first person's and was drawn until some other model was made. Now only 2048, 2049.
- **R_AddDObjToScene is 0x1405C9620** (model, pose, entity number, flags, ...; thunk 0x1405C9610). A model with one
  part and no skeleton work goes in as a loose scene model (0x88 entry at 0x14F92D020: +8 the part, +0x10 the model),
  the rest as scene models proper (0xB0 at 0x14F916008, count 0x14F915FFC, limit 0x200).
- **Worker commands, by the game's own names** (table of names 0x1409E7420, same order as the jump table):
  0 physics 0x1403A1AF0, 1 cell dyn brush 0x14057B180, 2 cell dyn model 0x140583460, 3 cell scene ent 0x140583B50,
  4 dpvs ent 0x140583600, 5 bound ent 0x1405B4270, 6 spot shadow ent 0x1405D7CB0, 7/8 trace 0x1403A8D50,
  9 fx pass 0 0x1402BAF20, A fx pass 2 0x1402B3010, B glass 0x1402A0700, C fx pass 4 0x1402B3210, D fx pass 5 0x1402B3360,
  E cell static 0x140585420, F umbra static 0x140062320, 10 add bsp 0x140569EB0, 11 add staticmodel 0x14056B110,
  12 add bsp sun 0x14056A2F0, 13 add staticmodel sun 0x14056D940, 14 skin model 0x1405B57A0, 15 add scene ent 0x140579B90,
  16 gen drawsurfs 0x1405CBD30, 17 cell glass 0x14029B240, 18 glass light 0x14029CCD0, 19 glass verts 0x14029C1A0,
  1A mark verts 0x1402A5510, 1B dyn mark verts 0x1402A5410, 1C fx trans verts 0x140288F20, 1D reactive motion 0x14056FFD0,
  1E fx shadow verts 0x140288E50, 1F fx verts 0x140288FA0, 20 compute cmds 0x140058100, 21 stream tabulation 0x14027F8A0,
  22 draw 0x1405870D0, 23 lui gc.
- **Glass:** "glass verts" 0x14029C1A0(command) keeps [command+0x38] and [command+0x60] and goes to Glass_GenerateVerts
  0x14029D6B0(system, list, command): the list has 8-byte entries, their number at +0x7000 (limit 0x300), each drawn by
  Glass_DrawPiece 0x14029D450; the number is set to 0 at the end. With the map not shown the hook sets it to 0 first.
  Limit warning function 0x1405E4050(kind, limit).

## 0.68: effects as clips (in the game exactly while the time is in the clip), kinds in the timeline (untested when written)
- User on 0.67: glass no longer mentioned (log: thousands of glass pieces held back a frame; 1 first-person model). Asked: scorestreak-ready
  popup hidden; effects grouped by kind in the timeline and selectable as one; spawning made predictable; red boxes still there; stretching bugged.
- Why effects "stopped spawning": a seek while paused, and every going back, plays the recording from the start again, which restarts the client
  game and with it the FX system: everything placed was gone, and effects.cpp only played an effect on the time ENTERING its clip (g_inside),
  so a seek inside a clip never played it again. Nothing ever took an effect away either (rain went on past its clip, and "Again every" piled
  looping rain up).
- FX_PlayOrientedEffect only queues a spawn command (0x1402AC050 = FX_InitSpawnCmd): no handle comes back. Taking away is by kind:
  **FX_KillEffectDef 0x1402B1880(client, def)** = r8d -1 then **FX_KillEffectDefNewerThan 0x1402B1890(client, def, msec)**: appends {def, msec}
  to a list of 64 a frame in the system's buffer ([system+0x148]: list at +0x102980 + (which*64 + n)*16, count +0x10B99C, which +0x10B980;
  no bounds check; system flags [system+0x140] & 0x10). Game's uses: 0x1401C43BA (explosions newer than now - a window), CG_RewindKillcamEffects
  0x1401A73B0: kill def, FX_RewindTo 0x1402B3FF0(client, time, x), then FX_PlayOrientedEffect with OLD begin times (so a begin time in the
  past is the game's own way of putting an effect back as it was).
- 0.68 model (effects.cpp): wanted = on, loaded, time in [at, at+length). Per placed effect Live{alive, def, rep, first_begin, last_begin, hold,
  dirty}. Each settled frame (after 3 settled frames): not wanted but alive -> kill def newer than first_begin-1 (others of ours of that kind
  begun no earlier are marked gone and begun again 2 frames later); wanted and not alive -> play with begin = game time of the clip's start
  (at most 20 s ago). TheaterState.rewinds (counted in go_back_to) marks everything gone. Changed by the editor -> begun again 200 ms after
  the last change. "Play it now" outside the clip shows it 2..10 s. Log lines "effects: ... begun/taken away", and once a kind what it is
  made of (FxEffectDef +0x14/+0x18/+0x1C counts, +0x38 parts of 0x140: +0x08 looping every/times, +0x2C delay, +0x34 life, +0xB4 kind,
  +0xB6 visuals, +0xD0 visual(s); material name first in a material).
- Red boxes, most likely: zone\s1iris_rain.ff holds the 37 effects only (43 KB). Their materials (distort_rainfloorsheet_atlas_s30,
  fx_water_dissipate_1_blend_atlas_z100_god_tip, fx_drops_add_atlas_flit, distort_rain_ripples_atlas_s20, fx_rain_ripple_add_atlas_flit,
  cloud_rain_wavy_thin_add_atlas_outdr_flit, fx_drops_add_atlas_z120_flit_outdr, fx_wind_rain_add_fbatlas_z512 ...) are looked up by name
  when the file loads; on a map that has not got one the game puts its default material there. The dump's images are 260-byte stubs
  (streamed from mp_refraction's own image file), so adding the materials to the zone needs the image data too: NOT done.
- HUD: the scorestreak-ready popup is a part with no id ("static.LUIElement" > LUIElement > LUIImage + 2 LUIText) added to the static layer
  when earned. Lua: the layers' addElement wrapped (hidden as it is added) and a every-frame timer that only re-hides; Look > HUD row
  "Scorestreak ready popup", included in "Kills only".
- Timeline: rows scroll (wheel over the names, bar at the right); effects by kind (one row, one bar over all clips; opened out, a row each);
  selection (click, Ctrl+click, right-click menu, Del); drags by the distance moved since the press (the old end-drag jumped: a short clip
  is drawn at least 10 px wide, and the length was set to where the mouse was); a start handle too. Up to 128 effects (was 32).

## 0.69: actors, handles on the picture (untested when written: built blind, from the disassembly)
- User (away from the PC): wants actors like T5 Iris (right click > add actor, outfits like the swapper, named actor1.., animations
  stitched with "Then" and blended, a death-animation library with a small bone view), x/y/z arrows under every effect and actor to slide
  them on the picture (only while the mouse is over it, right button up), and turning them.
- T5's way (t5/src/Actors.cpp) ported: own DObj + own XAnim tree, weights/times set each frame, R_AddDObjToScene with a client entity as pose.
- **XAnim on PC** (cluster 0x1404E3000..0x1404E9000): XAnimCreateAnims 0x1404E5000(name, count, alloc) = alloc(count*16+8), [0]=count;
  XAnimBlend 0x1404E3540(anims, index, name, firstChild, numChildren, flags, byte); XAnimCreate 0x1404E4FC0(anims, index, name) (DB_FindXAssetHeader
  0x14026F0F0(5, name, 1)); XAnimCreateFromParts 0x1404E5020(anims, index, parts); XAnimCreateTree 0x1404E5150(anims, alloc) = alloc(16): [0] anims,
  word [8] root info index; XAnimFreeTree 0x1404E5E70(tree, free); XAnimFreeInfo 0x1404E5CC0(tree, info, 0); XAnimCreateSimpleTree 0x1404E50E0(name),
  SimpleBlendTree 0x1404E5040, free 0x1404E5E40 / 0x1404E5E10; XAnimPlaySimpleTreeAnim 0x1404E7350, ...BlendTreeAnim 0x1404E70F0;
  **XAnimSetGoalWeight 0x1404E8420(obj, index, weight, goalTime, rate, notifyName, notifyType, restart)** (weights under 0.001 are 0);
  XAnimGetInfoIndex_r 0x1404E6390(tree, index, rootInfo); XAnimRestart 0x1404E7BE0; XAnimSetGoalWeightInternal 0x1404E87E0; XAnimAllocInfo-with-parents
  0x1404E32D0(obj, index, 0); XAnimGetAbsDelta 0x1404E5EB0(anims, index, rot[4], trans[3], time). Infos: 0x48 bytes from 0x14B5B9E00 (+2 notify index,
  +0x28 time, +0x2C old time, +0x30 cycle count). Entry (16 bytes from anims+8): word children count, word parent, +8 parts | (word flags, word first child).
  DObjSetTree 0x1404DECD0(obj, tree): [obj+0] = tree. DObjGetTree 0x1404DDFB0. XAnimParts: +0xE frames, +0x10 flags (1 loops), +0x30 framerate.
- **CG_General is around 0x1401910C0**: CG_PreProcess_GetDObj 0x1401928E0(…, entnum, eType, model, 0); cgs.gameModels 0x141646E18; pose = the centity
  itself (0x288 from 0x141655F60): origin +0x20 stored XORed, each 32 bits with the dword at 0x150D160B9 (0x67B0F0DA in the dump; the copy routine
  0x150C1D673(out, in, 12) traced through its obfuscation: out[i] = in[i] ^ byte (i&3) of it; 0x1401911C0 is NOT it), angles +0x44, number word +0x144, eType byte
  +0x150, model index +0x154; R_AddDObjToScene thunk 0x1405C9610(obj, pose, entnum, renderFlags, lightingOrigin, 0.0f, 0); render flags from 0x1401914E0.
  CG_MAKE_DOBJ 0x1401A2BF0(models, count, tree, handle, client, entnum) also resets 12 bytes at 0x141799F60[(client*2048+entnum)*12].
- Console names: Com_Restart does XAnimShutdown/XAnimInit/DObjInit; CL_InitCGame does Com_InitDObj (so a rewind frees every client DObj but NOT the
  XAnim infos): actors_release() runs in go_back_to before that.
- actors.cpp: handles 1984.. (preview 1983); outfit = a player's last model list kept by NAME (models_outfit_of / models_make_actor in models.cpp, which
  goes through CG_MAKE_DOBJ and so through hooked_create: swaps, sets, random outfit work by handle; models_randomize_one). Clips: at, length, speed,
  blend, loop; the last clip begun plays, the one before blends out; root motion from XAnimGetAbsDelta summed over the clips begun. Preview figure for
  the library's bone view (CG_DObjGetWorldTagMatrix on 22 bones, not added to the scene). passes.cpp counts actors' models as characters.
- ui.cpp: Scene > Actors, animation library (modal) with bone view, actor rows in the timeline, handles on the picture (draw_handles: arrows / rings,
  R or the Slide/Turn button), camera_view() gives the view for projecting.
