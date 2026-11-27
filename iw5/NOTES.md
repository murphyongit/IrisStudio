# IW5iris working notes (Modern Warfare 3, 2011)

The user first asked for MW2 (iw4/, built and installed but switched off with IW4iris.off, never run), then said he
meant MW3 and that it "should work for every version". Versions on his PC (27 Nov 2026):

- **Plutonium IW5** (%LOCALAPPDATA%\Plutonium, storage\iw5 exists; Plutonium keeps its own game executables in
  games\, so Steam's update did not touch it). 32-bit. Covered by this module.
- **Steam, 64-bit**: C:\Program Files (x86)\Steam\steamapps\common\Call of Duty Modern Warfare 3\iw5mp.exe is a NEW
  x64 build (PE 0x8664, built 2026-08-06, pdb C:\trees\iw5\game\pc\iw5mp_ship.pdb, base 0x140000000, a Steam
  ".bind" section, still imports d3d9.dll; also bink2w64, X3DAudio1_7, WINHTTP, bcrypt). Installed 2026-09-22.
  NOT covered: needs core built for x64 (core has no inline assembly, so that looks possible) and a module with
  every address and structure offset found again (x64 layouts differ wherever there is a pointer), no naked
  stubs (MinHook on whole functions; x64 has one calling convention). It is the VAC-protected online game.
- The old 32-bit Steam iw5mp.exe (1.9.461, what upstream IWXMVM supports and what he ran until April 2026 with
  ReShade 4.9.1 as d3d9.dll) is gone from his folder; other people still have it. Covered by this module.
- His demos: main\demo\*.demo in the Steam folder (names like x006a_6927c941.demo, 2025; a few named ones).
  MW3's window class is "IW5".

## 0.1: the 32-bit game (built blind on 27 Nov 2026: NOTHING here has been run)
- iw5/src is upstream IWXMVM's iw5 module (github.com/reallyluckyy/IWXMVM, commit 9758c12 of 2026-06-28), which
  compiles against this fork's core unchanged but for: Quit() (the fork's one more pure virtual); the entry point
  refuses online Plutonium like T5iris's; an unknown game version no longer ends the game (it threw away the
  process with ExitProcess: now a message and an exception Mod::Initialize catches, before anything is hooked);
  GetUserDirectory() is the game's folder (fs_basepath) under Plutonium, so main\demo is found and the mod's
  "iris" folder is not put into Plutonium's bin.
- It finds its addresses by signatures (Signatures.hpp), uses the game's own theater (cl_demo_play, the
  clientDemoPlayback block, demos with a 12320-byte header and 596-byte footer), no feature flags yet.
- iw5/launcher builds IW5iris.exe: with no game running it starts Plutonium in LAN mode
  (bin\plutonium-bootstrapper-win32.exe iw5mp "<game>" -lan -name "<name>", working folder = Plutonium's), waits
  for the "IW5" window, 15 s more for the menu, then LoadLibraryW of IW5iris.dll next to it. A running game is
  used as it is, except: online Plutonium (no -lan on its command line), iw5sp.exe, or a 64-bit process: refused.
  The 32-bit Steam game asks for a typed Y first (VAC). IW5iris.ini next to it: game=, plutonium=, name=.
- Build: push main -> Actions iw5.yml -> release iw5iris-latest (IW5iris.dll + IW5iris.exe, prerelease).
  build.yml (BO1) ignores iw5/**; see iw4/NOTES.md for the [skip ci] way of not republishing BO1.
- Core's log under Plutonium: IWXMVM.log in the process's working folder = Plutonium's folder.
- Local check: /tmp/mod_check.sh iw5 (see iw4/NOTES.md).

Licence: GNU GPL v3, as the rest of this repository; the module is upstream IWXMVM's work.

## 0.1 first try by the user (27 Nov 2026) and the starter's second build
- He ran IW5iris.exe: Plutonium answered "invalid game folder ... Unable to load import '_AIL_set_redist_directory@4'
  from module 'mss32.dll'". That is Steam's September 2026 update: his folder now holds the 64-bit game (no mss32.dll,
  no binkw32.dll, 64-bit fastfiles), which Plutonium IW5 cannot use. His Plutonium folder has no game-files\iw5-legacy.
- Plutonium's own answer (forum.plutonium.pw/topic/46446, "MW3 Downgrade and Update Prevention Guide"): the old
  32-bit depots through Steam's console (steam://open/console): download_depot 42680 42682 2661317971072643596
  (base), download_depot 42680 42683 1595601894688570808 (English), download_depot 42690 42691 4104640605720756125
  (multiplayer binaries; 42681 = singleplayer's, not needed). They land in <Steam>\steamapps\content\app_N\depot_N
  and are merged into one folder. (Other posts give other manifests for 42682/42683: 6067691797475126381 and
  6381783964590594065. Not checked by me: the forum could not be fetched from here.)
- The starter now tells a 32-bit game folder (has mss32.dll) from the 64-bit one, looks for the 32-bit files in
  steamapps\content\app_42680|42690|42682\depot_42682 and <game>\downgrade by itself, and explains the 64-bit case
  in its own words instead of leaving it to Plutonium's message.

## First working run (27 Nov 2026) and its four fixes (all from the 32-bit iw5mp.exe 1.9.461, md5 607fb4af...)
The user's downgraded depot's iw5mp.exe has the addresses Plutonium runs (MainWndProc 0x5D0600 in both), so it is
the reference binary: /home/claude/iw5/bin32, tools/x.py (disassembler) and tools/sig.py (resolves Signatures.hpp).
- Rewind went to the demo's start and stayed there: CL_AdjustTimeDelta (0x4851F0) runs in the frame after a snapshot
  was read (cl.newSnapshots, 0x1065AF4) and resets the clock when it is >500 ms from the snapshot, undoing the
  realtime jump the core makes after a rewind. Hooks/Playback.cpp clears newSnapshots when realtime jumped.
  (clientStatic's realtime is at 0xBA4EE8, 4 before the struct's field; ResetClientData no longer writes to it.)
- Timescale under 0.1 looked low-fps: whole milliseconds only. CG_SetFrameInterpolation (0x44BB80) is hooked and
  given the part of a millisecond the frame is at. Places players and the view; animations and effects still step.
- Bone camera ended the game: bones were asked for on the editor's thread. Bones.cpp answers on the main thread, in
  CL_Demo_CalcViewValues (0x6AAA60), right after the game resets its skeleton memory (0x48DE00, in CG_CalcViewValues).
  That function only runs in the theater's free/third-person modes, which the mod's cameras are.
- Free camera spun: Plutonium's window procedure and raw input come back for the new window after the mod's
  vid_restart; they are now removed each time the editor takes the mouse, and SetCursorPos from any other module is
  refused meanwhile (CursorGuard.cpp, logs the first caller).
- The log is copied by the starter to <mod folder>\IW5iris_log.txt when the game closes.
