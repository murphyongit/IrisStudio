param([string]$Out = (Join-Path (Split-Path $PSScriptRoot -Parent) "bin\release"), [switch]$NoBuild, [switch]$Beta)
# Makes the release zip, IrisStudio-<version>.zip, from a release build: IrisStudio.exe
# (with T5iris.dll in it), LICENSE and NOTICES.txt (the third-party licenses: NOTICES-header.txt, then each license's
# text from core/third-party, and the fonts' OFL). Builds t5 and the launcher in Release first unless -NoBuild, and
# refuses a T5iris.dll built with the harness. Nothing is uploaded: publishing a release is a step of its own.
# -Beta makes IrisStudio-<version>-beta.zip from a beta build (/p:IrisBeta=true, in the ReleaseBeta folders),
# which takes only keys of the user's key pair: it refuses before
# tools\beta-keys\beta-keys.ps1 init has written the user's public key, or when that is the test pair's.
$ErrorActionPreference = "Stop"
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$version = (Select-String -Path "$root\launcher\src\Version.hpp" -Pattern 'IRIS_STUDIO_VERSION_STRING "([^"]+)"').Matches[0].Groups[1].Value
$folder = if ($Beta) { "ReleaseBeta" } else { "Release" }

if ($Beta) {
  $keys = Get-Content "$root\core\src\Beta\BetaKeys.hpp" -Raw
  $public = [regex]::Match($keys, 'PublicKey PUBLIC_KEY = \{([^}]*)\}').Groups[1].Value -replace '\s', ''
  $test = [regex]::Match($keys, 'PublicKey TEST_PUBLIC_KEY = \{([^}]*)\}').Groups[1].Value -replace '\s', ''
  if (-not $public) { throw "there's no public key yet: run tools\beta-keys\beta-keys.ps1 init first" }
  if ($public -eq $test) { throw "PUBLIC_KEY is the test pair's: a beta build has to take the user's keys" }
}

if (-not $NoBuild) {
  $msbuild = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe"
  if (-not (Test-Path $msbuild)) { $msbuild = (Get-Command msbuild).Source }
  $switch = if ($Beta) { "/p:IrisBeta=true" } else { "/p:IrisBeta=" }
  & $msbuild "$root\IWXMVM.sln" /t:launcher /p:Configuration=Release /p:Platform=x86 $switch /m /v:minimal /nologo |
    Where-Object { $_ -match "error|warning C" }
  if ($LASTEXITCODE -ne 0) { throw "the build failed" }
}

# the DLL the exe embeds has to be a release one: a harness build has the harness's strings in it
$dll = "$root\t5\bin\Win32\$folder\T5iris.dll"
if (Select-String -Path $dll -Pattern "IWXMVM_HARNESS_DIR" -SimpleMatch -Quiet) { throw "T5iris.dll is a harness build" }
# and a beta build has the check's words in it, a default build doesn't
$checks = Select-String -Path $dll -Pattern "needs a key for this PC" -SimpleMatch -Quiet
if ($Beta -and -not $checks) { throw "T5iris.dll in $folder isn't a beta build" }
if (-not $Beta -and $checks) { throw "T5iris.dll in $folder is a beta build" }
$exe = "$root\launcher\bin\Win32\$folder\IrisStudio.exe"
if ((Get-Item $exe).LastWriteTime -lt (Get-Item $dll).LastWriteTime) { throw "IrisStudio.exe is older than T5iris.dll" }

$stage = Join-Path ([IO.Path]::GetTempPath()) "IrisStudio-package-$([Guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Force $stage, $Out | Out-Null
try {
  Copy-Item $exe "$stage\IrisStudio.exe"
  Copy-Item "$root\LICENSE" "$stage\LICENSE"
  $third = "$root\core\third-party"
  $licenses = [ordered]@{
    "Dear ImGui" = "$third\imgui\LICENSE.txt"
    "spdlog" = "$third\spdlog\LICENSE"
    "MinHook" = "$third\minhook\LICENSE.txt"
    "OpenGL Mathematics (GLM)" = "$third\glm\copying.txt"
    "JSON for Modern C++" = "$third\json\LICENSE.MIT"
    "tinyobjloader" = "$third\tinyobjloader\LICENSE"
    "magic_enum" = "$third\magic_enum\LICENSE"
    "incbin" = "$third\incbin\UNLICENSE"
    "IconFontCppHeaders" = "$third\IconFontCppHeaders\licence.txt"
    "ReShade's add-on headers" = "$third\reshade\LICENSE.md"
    "Rubik and Font Awesome 6 Free (fonts)" = "$PSScriptRoot\OFL-1.1.txt"
  }
  $text = @(Get-Content "$PSScriptRoot\NOTICES-header.txt")
  foreach ($name in $licenses.Keys) {
    $text += ""; $text += ("=" * 100); $text += $name; $text += ("=" * 100); $text += ""
    $text += Get-Content $licenses[$name]
  }
  Set-Content -Path "$stage\NOTICES.txt" -Value $text -Encoding utf8
  $zip = Join-Path $Out "IrisStudio-$version$(if ($Beta) { '-beta' }).zip"
  Compress-Archive -Path "$stage\*" -DestinationPath $zip -Force
  $hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant()
  "$zip ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB), SHA-256 $hash"
}
finally {
  Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
}
