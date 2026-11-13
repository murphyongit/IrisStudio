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
- CL_LoadTransientByXModel(name) 0x140026A20 (FindTransientAssetIndices 0x140025470, LoadTransientByIndex 0x1400268D0). Calling it directly
  evicts models in use -> crash (0.44). Proper fix = change the player's customization so the game streams it itself.
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
