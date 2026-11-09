param([string]$Configuration = "Release")
# Builds and runs the tests of the core logic that doesn't need the game (tests/actors-tests). Exits with 1 if any fail.
$root = Split-Path $PSScriptRoot -Parent
$msbuild = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe"
if (-not (Test-Path $msbuild)) {
  $msbuild = (Get-Command msbuild -ErrorAction SilentlyContinue).Source
}

& $msbuild "$root\IWXMVM.sln" /t:actors-tests /p:Configuration=$Configuration /p:Platform=x86 /m /v:minimal /nologo |
  Where-Object { $_ -match "error|warning C" }
if ($LASTEXITCODE -ne 0) { "build failed"; exit 1 }

& "$root\tests\bin\Win32\$Configuration\actors-tests.exe"
exit $LASTEXITCODE
