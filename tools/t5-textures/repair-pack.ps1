<#
Repairs this PC's texture pack: Iris Studio checks every image of the pack
against the game's, then makes only the wrong ones again, at the pack's own scale. The rest of the pack isn't touched,
and images already repaired aren't made twice, so it can be stopped with Ctrl+C and run again to carry on.

    powershell -ExecutionPolicy Bypass -File repair-pack.ps1 [-IrisStudio <path>] [-CheckOnly]

It needs an Iris Studio built on or after 2026-11-04, beside this script or named with -IrisStudio. On a pack built by an
older Iris Studio it finds about a third of the images wrong. On an RTX 4070 Laptop a whole 2x pack took 45
minutes; about 1,000 images go through the model, the rest are resized on the processor, both at once. A 4x pack takes
about the same on the card and longer on the processor. Close the game first. -HomeFolder and -Storage are for tests, in
place of Iris Studio's own folder and the mod's.
#>
param([string]$IrisStudio = "", [switch]$CheckOnly, [string]$HomeFolder = "", [string]$Storage = "")

$ErrorActionPreference = "Stop"
if (-not $IrisStudio) { $IrisStudio = Join-Path $PSScriptRoot "IrisStudio.exe" }
if (-not (Test-Path $IrisStudio)) {
    Write-Host "No IrisStudio.exe beside this script. Put the new one here, or pass -IrisStudio <path>." -ForegroundColor Red
    exit 2
}
if (Get-Process -Name "plutonium-bootstrapper-win32" -ErrorAction SilentlyContinue) {
    Write-Host "The game is running. Close it first." -ForegroundColor Red
    exit 2
}
$iris = if ($HomeFolder) { $HomeFolder } else { Join-Path $env:LOCALAPPDATA "IrisStudio" }
$log = Join-Path $iris "logs\textures.log"
$from = if (Test-Path $log) { (Get-Item $log).Length } else { 0 }

$arguments = @(if ($CheckOnly) { "--check-textures" } else { "--repair-textures" })
if ($HomeFolder) { $arguments += "--home", "`"$HomeFolder`"" }
if ($Storage) { $arguments += "--storage", "`"$Storage`"" }
Write-Host "Reading the game and checking the pack. The first read takes about 2 minutes."
$process = Start-Process -FilePath $IrisStudio -ArgumentList $arguments -PassThru

# what the log says as it goes, without the list of images
function Show-Log {
    if (-not (Test-Path $log)) { return }
    $stream = [IO.File]::Open($log, 'Open', 'Read', 'ReadWrite,Delete')
    try {
        [void]$stream.Seek($script:from, 'Begin')
        $reader = New-Object IO.StreamReader($stream)
        $text = $reader.ReadToEnd()
        $script:from = $stream.Position
    } finally { $stream.Dispose() }
    foreach ($line in ($text -split "`r?`n")) {
        if ($line -and $line -notmatch ' (right|wrong): ' -and $line -notmatch ': 0 textures to build') { Write-Host $line }
    }
}

try {
    while (-not $process.HasExited) {
        Start-Sleep -Seconds 2
        Show-Log
    }
    Show-Log
} finally {
    if (-not $process.HasExited) {
        # Ctrl+C: what is made stays, and running this again carries on
        Stop-Process -Id $process.Id -Force
        Write-Host "Stopped. Run this again to carry on."
    }
}
$process.WaitForExit()
switch ($process.ExitCode) {
    0 { Write-Host "Done." -ForegroundColor Green }
    2 { Write-Host "Iris Studio didn't find the game. Open it once and pick the game's folder." -ForegroundColor Red }
    default { Write-Host "Some images are still wrong. The log says which: $log" -ForegroundColor Yellow }
}
exit $process.ExitCode
