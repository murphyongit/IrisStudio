# T5 texture packs

Tools for Black Ops (T5) texture packs: the research's prototype tools made durable. Iris
Studio builds packs itself (`launcher/src/TexturePack.cpp`); these are the reference to check its output against, and
make small packs for the harness (`texpack`, scenario `visuals-texture-pack`). They read the user's own game files and
write nothing into the game folder.

- `iwi.py`: the game's `.iwi` (version 13) files read and written back byte for byte (the 48-byte header, the levels
  smallest first; past the last level `fileSizeForPicmip` keeps the smallest), the iwds' image index in the game's search
  order (Plutonium's `storage\t5\main` first, then `main`, the iwd named last winning), and DDS (DXT or 32-bit).
- `zone.py`: a zone's images, read from its image records without OpenAssetTools, and their kinds (the same rules as
  `TextureLogic::GetKind`). `python zone.py <zone.ff> --game <Black Ops folder>`
- `roundtrip.py`: every flat DXT image of a zone written back; all of Nuketown's 996 and common_mp's 1,154 come back
  byte for byte. `python roundtrip.py <zone.ff> --game <Black Ops folder>`
- `build.py`: a pack built from the command line into `<out>\v1` with its `index.json`, the layout Iris Studio writes
  and the mod reads: colour and normal maps through the model (`--esrgan`, `--models`: realesrgan-ncnn-vulkan with
  4xNomos8kSC) or Lanczos (`--plain`), spec maps Lanczos, normal maps rebuilt as RGB, renormalized and packed back, then
  texconv to the game's DXT format with mips (`--texconv <path>`, or `auto` for the one Iris Studio downloaded), alpha
  apart and a spec map keeping the game's levels, or 32-bit with box mips (`--uncompressed`). `--old-mips`
  builds the old way, for `visuals-texture-lighting`'s control.

  The harness's small pack (the planter's brick at 2x, about 11 MB):

  ```
  python -I build.py --game <Black Ops folder> --out <folder>\packs\small --zones mp_nuked --scale 2 --plain --uncompressed --only "~-geb_art_wall_redbrick_c" eb_art_wall_redbrick_n
  ```

- `check.py`: a pack's images against the game's, by kind and format, as Iris Studio's check (`--check-textures`,
  `TextureBuild::Compare`, `IsWrong`): `python -I check.py <v1 folder> --game <Black Ops folder> [--list]`, exit 1 when any is wrong. It writes
  nothing. A pack of an older build lists its wrong images here (a 2x pack of that time: 5,366 of 16,298).
- `dxt.py`: DXT levels decoded with numpy, one level of a DDS read alone.
- `repair-pack.ps1`, `repair-pack.cmd`: Iris Studio's repair of this PC's pack (`--repair-textures`), for a pack built
  before 2026-11-04: the check, then only the wrong images made again, at the pack's own scale. Beside an `IrisStudio.exe`
  of that build or later, double-click the `.cmd`. Stopped, it carries on where it was.
- `downscale-pack.ps1`: a 2x pack from a 4x one Iris Studio built, with no model run and no tools: each 4x texture's
  second mip level is that texture at half the size (texconv made it from the full-quality upscale), so the 2x file is
  the 4x file from that level down, byte for byte; textures already at 2x (the 4,096 cap) are copied. A 4x pack of an
  older build hands its wrong levels on: repair it first. It writes the
  index.json with the 2x scales, sizes and bytes and each map's scale, so it can replace `textures1` or be imported.
  Windows PowerShell 5.1 or PowerShell 7, nothing else (33 textures in about a second here).

  ```
  powershell -ExecutionPolicy Bypass -File downscale-pack.ps1 -From <the 4x pack's v1 folder> -To <a new folder>
  ```

Python 3 with numpy and Pillow. (`downscale-pack.ps1` needs only PowerShell.) realesrgan-ncnn-vulkan, the model and texconv are the downloads Iris Studio pins;
 the tools don't download anything.
