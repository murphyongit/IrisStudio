# IW4iris

Iris for Call of Duty: Modern Warfare 2 (2009) multiplayer as the IW4M client runs it (`iw4m.exe`): the same
editor as T5iris (Black Ops), on the same core, for the game's own `.dm_13` demos.

Two files go into the game's folder, next to `iw4m.exe`:

- `IW4iris.dll`: the mod
- `ddraw.dll`: what makes the game load it (it passes everything else on to Windows' own ddraw.dll)

Start the game as usual and play a demo. To switch the mod off without deleting anything, put an empty file
named `IW4iris.off` in the same folder.

Working notes for whoever continues this: `NOTES.md`.
