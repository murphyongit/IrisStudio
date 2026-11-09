param(
  [Parameter(Mandatory = $true)] [string]$Game,
  [string]$Out = (Join-Path $env:LOCALAPPDATA "Plutonium\storage\t5\iris\zones"),
  [string]$Pack = "",
  [string]$Tools = (Join-Path $env:LOCALAPPDATA "IWXMVM\tools"),
  [string[]]$Only = @(),
  [switch]$AnimationsOnly
)
# Builds the character zones the mod loads its actors' characters from: a small zone for
# each character of the campaign and Zombies, which OpenAssetTools' Linker copies out of this game's own zones (its
# models and what they draw with; the textures stay in the game's iwd files), and the campaign maps' kits
# (tools/t5-maps/kits.py), and the campaign's and Zombies' weapons with the sound banks of
# their shots (tools/t5-weapons/weapons.py). Then the animation pack: every animation
# of the game's zones in one file the mod loads them from (animations.py). Run it once;
# again after a game update.
#   -Game   the Black Ops folder (with zone\Common)
#   -Out    where the mod looks for the zones: storage\t5\iris\zones of Plutonium (the default); for Steam, the
#           iris folder next to BlackOpsMP.exe, e.g. -Out "<game folder>\iris\zones"
#   -Pack   where animations.pack goes: the folder -Out is in (storage\t5\iris) by default
#   -Tools  where OpenAssetTools goes: its release v0.34.0 (oat-windows.zip, 3.8 MB, GPL-3) is downloaded there the
#           first time and checked against its SHA-256
#   -Only   character ids of core\resources\characters.json to build alone (e.g. khe_sanh/c_vtn_nva1), without the
#           animation pack
#   -AnimationsOnly  the animation pack alone (no OpenAssetTools needed)
# Needs Python 3 (python on the PATH). Takes a few minutes and about 500 MB (zones 270 MB, the pack 225 MB).
$ErrorActionPreference = "Stop"
$version = "v0.34.0"
$sha256 = "fd2aafd5a6a99d520f67c16ab6e12873da87e8de0b81312d239a1ee2db59d515"
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$catalog = Join-Path $repo "core\resources\characters.json"
if (-not $Pack) { $Pack = Split-Path $Out -Parent }

if (-not (Test-Path (Join-Path $Game "zone\Common"))) { throw "$Game has no zone\Common: give the Black Ops folder" }
$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) { throw "Python 3 isn't on the PATH (python.org, or the Microsoft Store's)" }

if (-not $AnimationsOnly) {
  # OpenAssetTools, once
  $oat = Join-Path $Tools "oat-$version"
  $linker = Join-Path $oat "Linker.exe"
  if (-not (Test-Path $linker)) {
    $download = Join-Path ([IO.Path]::GetTempPath()) "oat-download-$([Guid]::NewGuid().ToString('N'))"
    New-Item -ItemType Directory -Force $download | Out-Null
    $zip = Join-Path $download "oat-windows.zip"
    "Downloading OpenAssetTools $version..."
    Invoke-WebRequest -UseBasicParsing -Uri "https://github.com/Laupetin/OpenAssetTools/releases/download/$version/oat-windows.zip" -OutFile $zip
    $hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($hash -ne $sha256) { Remove-Item $download -Recurse -Force; throw "oat-windows.zip has SHA-256 $hash, not $sha256" }
    New-Item -ItemType Directory -Force $oat | Out-Null
    Expand-Archive -Path $zip -DestinationPath $oat -Force
    Remove-Item $download -Recurse -Force
  }

  $work = Join-Path ([IO.Path]::GetTempPath()) "t5-characters-$([Guid]::NewGuid().ToString('N'))"
  New-Item -ItemType Directory -Force $work, $Out | Out-Null
  $arguments = @("-I", (Join-Path $PSScriptRoot "zones.py"), "--game", $Game, "--linker", $linker, "--work", $work,
    "--out", $Out, "--catalog", $catalog)
  if ($Only.Count) { $arguments += @("--only") + $Only }
  try {
    & $python.Source @arguments
    $failed = $LASTEXITCODE -ne 0
  }
  finally {
    if (-not $failed) { Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue }
  }
  if ($failed) { "Some zones weren't built: the Linker's logs are in $work"; exit 1 }

  # the campaign maps' kits: the players' scripts and models, the RC-XD, bomb and HQ
  # crate of a multiplayer map of each teamset, which a campaign level loads with
  if (-not $Only.Count) {
    $kitWork = Join-Path ([IO.Path]::GetTempPath()) "t5-kits-$([Guid]::NewGuid().ToString('N'))"
    & $python.Source -I (Join-Path $repo "tools\t5-maps\kits.py") --game $Game --linker $linker --work $kitWork --out $Out
    if ($LASTEXITCODE -ne 0) { "The campaign maps' kits weren't all built: the Linker's logs are in $kitWork"; exit 1 }
    Remove-Item $kitWork -Recurse -Force -ErrorAction SilentlyContinue
    # and the pictures of the Change Map menu's campaign and Zombies maps, beside the zones
    $pictureWork = Join-Path ([IO.Path]::GetTempPath()) "t5-pictures-$([Guid]::NewGuid().ToString('N'))"
    & $python.Source -I (Join-Path $repo "tools\t5-maps\pictures.py") --game $Game `
      --unlinker (Join-Path (Split-Path $linker) "Unlinker.exe") --work $pictureWork --out (Join-Path (Split-Path $Out) "pictures")
    if ($LASTEXITCODE -ne 0) { "Some of the Change Map menu's pictures weren't copied"; exit 1 }
    # the campaign's and Zombies' weapons and the sound banks of their shots
    $weaponWork = Join-Path ([IO.Path]::GetTempPath()) "t5-weapons-$([Guid]::NewGuid().ToString('N'))"
    & $python.Source -I (Join-Path $repo "tools\t5-weapons\weapons.py") zones --game $Game --linker $linker `
      --work $weaponWork --out $Out
    if ($LASTEXITCODE -ne 0) { "The weapons' zones weren't all built: the Linker's logs are in $weaponWork"; exit 1 }
    Remove-Item $weaponWork -Recurse -Force -ErrorAction SilentlyContinue
  }
}

if (-not $Only.Count) {
  "Writing the animation pack..."
  & $python.Source -I (Join-Path $PSScriptRoot "animations.py") pack $Game $Pack
  if ($LASTEXITCODE -ne 0) { "The animation pack wasn't written"; exit 1 }
}
"Done: in the game, the Actors tab's Look page loads each character as it is chosen ('Check again' in its Set up... if the game is running)."
