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
