# IW4iris working notes (Modern Warfare 2 2009 multiplayer, build 177, image base 0x400000)

## What it is
- The user's game: D:\GAMES\MW2, started with **iw4m.exe + iw4m.dll** (IW4M 4.1.449, a fourDeltaOne-descended client, built 2016).
  iw4m.exe's code section is byte-for-byte the code of iw4x.exe (same folder): the plain iw4mp build 177. So every
  address in the IW4x client's source applies (github.com/iw4x/iw4x-client, tag r5318 is the last 32-bit one; `main`
  there has moved to a 64-bit game and other addresses). `iw4/src/Game/Structs.hpp` is that client's Structs.hpp.
- Unlike S1iris (its own x64/Direct3D 11 editor, because Advanced Warfare has no demos and is 64-bit), IW4iris is a
  **module for the shared core** (`core/`, what T5iris is built on): MW2 is 32-bit, Direct3D 9 and plays its own
  .dm_13 demos, which is what core is made for. Template: upstream IWXMVM's iw3 and iw5 modules
  (github.com/reallyluckyy/IWXMVM). S1iris's extra features come over one at a time on top of core.
- Loading: `iw4/loader` builds **ddraw.dll** (iw4m.exe imports DirectDrawCreateEx/DirectDrawEnumerateExA from DDRAW.dll;
  nothing else in the folder uses that name; ReShade here is dxgi.dll). It forwards all 22 ddraw exports to
  Windows' own and loads IW4iris.dll when the exe is iw4m.exe. A file `IW4iris.off` next to it switches the mod off.
  IW4iris.dll waits for the Direct3D device (0x66DEF88), then 4 s, then starts core.
- Build/install loop: push `main` -> Actions `iw4.yml` (msbuild IWXMVM.sln /t:iw4, cmake for the loader) -> release
  `iw4iris-latest` (IW4iris.dll + ddraw.dll, prerelease) -> D:\GAMES\MW2\. `build.yml` (BO1) ignores iw4/**; a push
  that touches core/ republishes BO1's `latest` unless the commit says [skip ci] (then run iw4.yml by a second,
  iw4-only push).
- Local check before pushing: /tmp/iw4_check.sh (clang --target=i686-w64-mingw32 -fms-extensions -fasm-blocks
  -fsyntax-only against mingw's headers + core's; no -fms-compatibility, it breaks libstdc++). Offsets:
  /home/claude/iw4/offs/off.py Struct.field (Structs.hpp compiled with mingw, MS layout; all its static_asserts pass).
  Disassembly: /home/claude/iw4/tools/x.py d|f|c|r|s|b (capstone; c = callers, r = references, s = strings).

## Demo files (.dm_13), checked on the user's demos
- A run of messages, each beginning with a kind byte (CL_ReadDemoMessage 0x5A9CF0):
  0 = server message: int sequence, int length, body (CL_ReadDemoNetworkPacket 0x5A9BA0); 2 = the same with one more
  int after the sequence (never seen in his files); 1 = client archive (0x5A9AB0): int index, origin[3], velocity[3],
  int movementDir, int bobCycle, int serverTime, viewangles[3], int locationSelectionInfo, and float[2] only if that
  is not 0 (56 or 64 bytes after the kind byte). End: kind 0 with sequence -1 and length -1 (9 bytes).
- First message = gamestate (~50 KB: 53181, 51485). The first snapshot can be over 10,000 bytes (10290 in
  quarry2.dm_13): core's "10,000 or more is a gamestate" rule is wrong here -> GameInterface::IsDemoGamestateMessage
  (IW4: the file's first message, or 30,000 bytes or more).
- All reads go through clc.demoread (clc+0x401A0 = 0xA5EA18), set by the demo command to 0x5A8660, which puts
  clc.demofile into its third argument and jumps to FS_Read (0x4A04C0). Hooked there.
- iw4m.dll keeps pointers to 0x5A8660, CL_FirstSnapshot and FS_Read: it has demo controls of its own (cl_pauseDemo,
  cl_freezeDemo, cl_demoBackJump, cl_demoForwardJump, cl_demoFast/SlowTimescaleSpeed). Not to be used together with
  the editor's. Its demos: data\demos\*.dm_13 with a .meta beside each (name, map, gametype, length, time).

## Found so far (IW4x = from its source, dis = from the disassembly)
- cg 0x7F0F78 (cg_s 0xFD540; time +0x6A758, renderingThirdPerson +0x6A768, refdef +0x6A778: view at refdef+0x10 =
  0x85B700 {tanHalfFovX, tanHalfFovY, org 0x85B708, axis 0x85B714}, viewOffset 0x85B73C, refdefViewAngles +0x6E6D8);
  cgs 0x7ED3B8 (serverCommandSequence +0x14, processedSnapshotNum +0x18); cg_entities 0x8F3CA8 (0x204 each, 2048);
  clientinfo 0x8E77B0 (0x52C each, 18; inside cg); cl 0xB2C698 (snap +8: valid +0x311C, snapFlags, serverTime
  0xB2F7C4; serverTime +0x3158; serverTimeDelta +0x3164; snapshots +0x87DC x32); clc 0xA1E878 (serverCommands
  +0x20148 [128][1024], demoName +0x40150, demoplaying +0x40194, demoread +0x401A0, demofile +0x401A4);
  cls 0xA7FE90 (realtime +0x11C = 0xA7FFAC, skelTimeStamp +0x8D8, gameState +0x408EC = 0xAC077C, 0x240B0 bytes);
  clientUIActive 0xB2BB88 (keyCatchers +0x9B0, connectionState +0x9B8); con 0x9FCCF8; device 0x66DEF88;
  window 0x64A3AD0; fs_searchpaths 0x63D96E0; clientObjMap 0x1B288B0 (words), objBuf 0x1ADA8A8 (DObj 0x9C).
- Com_Frame calls Com_TimeScaleMsec 0x60B380(int msec) at 0x47DF84 (com_fixedtime 0x1AD7914, timescale dvar
  0x1AD7920, dev_timescale 0x1AD8F20, code time scale float 0x1AD7900, remainder 0x1AD78F8; never under 1), then
  SV_Frame 0x4BAA60(msec) and 0x48A0A0(msec, ..). Hooked for pause/slow motion (dis).
- CL_PlayDemo_f 0x5A86D0 ("demos/%s.dm_13", or the name as given when it already ends so); CL_Record_f 0x5A8170 (dis).
- CL_FirstSnapshot 0x5A1CD0, client number in ESI; its Con_TimeJumped call is at 0x5A1D30 (0x44C7E0) (dis).
- CG_DrawActiveFrame 0x486C20: CG_CalcViewValues 0x59A1E0 (no arguments; called only at 0x486D26), then 0x598AA0
  (called only at 0x486D2B), then refdef goes to 0x4AF430 (effects?), 0x4B96C0 (listener) ... The cameras' hook is
  the call at 0x486D2B (dis). CG_OffsetThirdPersonView 0x595F50, first person 0x596AC0, AnglesToAxis 0x4E2620 (dis).
- CG_DObjGetWorldTagPos 0x426580(pose, obj, tagName, pos); CG_DObjGetWorldTagMatrix 0x426660(pose, obj, tagName,
  axis[3][3], origin); both: DObjGetBoneIndex 0x504F20(obj, name, &index) -> 0x438670(pose, obj, index) ->
  0x4C4400(obj) = the bone matrices (32 bytes each; positions are relative to refdef.viewOffset) (dis).
- Cbuf_AddText 0x404B20(client, text); Dvar_FindVar 0x4D5390; Dvar_SetStringByName 0x44F060; Com_GetClientDObj
  0x41FF50(handle, client); SL_FindString 0x434EE0; SL_GetString 0x4CDC10; CG_ExecuteNewServerCommands
  0x4BE6E0(client, latest); IN_Frame 0x475E10; MainWndProc 0x4731F0; R_AddCmdDrawText 0x509D80; DB_FindXAssetHeader
  0x407930; DB_EnumXAssets 0x4B76D0; Sys_Milliseconds 0x42A660 (IW4x).
- Settings the game has (checked in the exe): cg_draw2D, cg_drawGun, cg_thirdPerson(+Range, Angle), cg_fov, cg_fovScale,
  ui_hud_hardcore, hud_enable, cg_drawShellshock, ui_drawCrosshair, cg_drawCrosshair, cg_drawDamageDirection,
  cg_drawDamageFlash, painVision*, cg_centertime, cg_overhead{Names,Rank,Icon}Size, con_gameMsgWindowN{MsgTime,
  LineCount,...}, g_TeamColor_{Allies,Axis,MyTeam,EnemyTeam}, r_dof_*, r_filmTweak*, r_filmUseTweaks, r_clear,
  r_clearColor, r_znear, r_zfar, r_fog, r_drawSun, r_fullbright, r_lockPvs, r_skipPvs, r_lodBias{Rigid,Skinned},
  sm_enable, sm_sunSampleSizeNear, r_smp_backend, fx_enable, fx_draw, timescale, com_maxfps, cl_ingame, cl_paused.
  NOT there: ui_hud_obituaries, r_tonemap*.

## 0.1 (27 Nov 2026; built blind: NOTHING here has been run in the game)
- The module as upstream's iw3/iw5 ones: demo command + Com_TimeScaleMsec + demo read hooks (pause, speed, rewind
  through core's Rewinding), the cameras (free, dolly, bone, first/third person) through the view hook, Look
  (sun from refdef.primaryLights, depth of field and film tweaks by dvars, HUD parts by dvars), entities and bones.
- Not yet: everything behind core's feature flags (animations, kills on the timeline, render size, loadouts,
  actors, shadow quality, surface depth, texture packs, sky), lights/props/effects, score popups and
  icons-and-text HUD toggles (kept as flags only), the compass on rewind (s_compassActors not found: core is given
  a dummy to clear), "connection interrupted" picture, the killcam "you" marker.
- If it does not start: the log is in the mod's folder under the game's (core's Logger); with no log at all the
  loader did not load IW4iris.dll (missing Visual C++ 2015-2022 x86 runtime or d3dx9_43.dll would do that).
