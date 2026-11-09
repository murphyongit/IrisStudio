<#
.SYNOPSIS
    Makes a 2x texture pack from a 4x one Iris Studio built, without running the model again.

.DESCRIPTION
    Each texture of a 4x pack is a DDS file with its mip levels, made by texconv from the full-quality upscale before
    compression. Its second level is that texture at half the size, so the 2x texture is the same file from its second
    level down: nothing is decoded or compressed again, and it is what the builder would make at 2x (which also halves
    the upscale), up to texconv's mip filter. Textures already at 2x or below (the 4,096 cap: a 2,048 texture is 2x in
    a 4x pack) are copied as they are.

    The new folder gets the pack's index.json with each image's scale, size and bytes, and each map's scale, so Iris
    Studio and T5iris take it as a 2x build: copy it over <the mod's folder>\textures\v1 (with T5iris closed), or keep
    it apart and use Iris Studio's Import... on it. Needs Windows PowerShell 5.1 or PowerShell 7, nothing else.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File downscale-pack.ps1 -From "D:\pack4x\textures\v1" -To "D:\pack2x\v1"
#>
param(
    # the 4x pack: the folder with index.json and the <image>.dds files (<the mod's folder>\textures\v1)
    [Parameter(Mandatory = $true)][string]$From,
    # a new folder for the 2x pack (made if missing; files of the same names in it are replaced)
    [Parameter(Mandatory = $true)][string]$To
)

$ErrorActionPreference = 'Stop'
$TargetScale = 2
$MaxSide = 4096  # TextureLogic::MAX_SIDE

$From = [IO.Path]::GetFullPath($From)
$To = [IO.Path]::GetFullPath($To)
if ($To.TrimEnd('\').StartsWith($From.TrimEnd('\'), [StringComparison]::OrdinalIgnoreCase)) {
    throw "The new folder can't be the 4x pack's folder or inside it: $To"
}
$indexFile = Join-Path $From 'index.json'
if (-not (Test-Path -LiteralPath $indexFile)) {
    throw "No index.json in $From. Point -From at the pack's folder (<the mod's folder>\textures\v1)."
}
$index = [IO.File]::ReadAllText($indexFile) | ConvertFrom-Json
if ($index.version -ne 1) { throw "This index is version $($index.version); the script knows version 1." }
[void][IO.Directory]::CreateDirectory($To)

# TextureLogic::GetScale: the largest of 4, 2, 1 at most the scale asked that keeps both sides within 4,096
function Get-Scale([int64]$width, [int64]$height, [int]$scale) {
    while ($scale -gt 1 -and ($width * $scale -gt $MaxSide -or $height * $scale -gt $MaxSide)) { $scale = $scale / 2 }
    return $scale
}

# the bytes of one level of a DDS file
function Get-LevelBytes([string]$fourCC, [int]$bits, [int64]$width, [int64]$height) {
    if ($fourCC -eq 'DXT1') { return [Math]::Max(1, ($width + 3) -shr 2) * [Math]::Max(1, ($height + 3) -shr 2) * 8 }
    if ($fourCC -eq 'DXT3' -or $fourCC -eq 'DXT5') {
        return [Math]::Max(1, ($width + 3) -shr 2) * [Math]::Max(1, ($height + 3) -shr 2) * 16
    }
    if ($fourCC -eq '') { return $width * $height * ($bits / 8) }
    throw "a DDS format the script doesn't know ($fourCC)"
}

# Writes the DDS file at $source from its level $drop down into $destination; returns its new width, height and bytes
function Copy-FromLevel([string]$source, [string]$destination, [int]$drop) {
    $in = [IO.File]::OpenRead($source)
    try {
        $header = New-Object byte[] 128
        if ($in.Read($header, 0, 128) -ne 128 -or [Text.Encoding]::ASCII.GetString($header, 0, 4) -ne 'DDS ') {
            throw 'not a DDS file'
        }
        $height = [BitConverter]::ToUInt32($header, 12)
        $width = [BitConverter]::ToUInt32($header, 16)
        $levels = [Math]::Max(1, [BitConverter]::ToUInt32($header, 28))
        $pfFlags = [BitConverter]::ToUInt32($header, 80)
        $fourCC = if ($pfFlags -band 0x4) { [Text.Encoding]::ASCII.GetString($header, 84, 4) } else { '' }
        $bits = [BitConverter]::ToUInt32($header, 88)
        if ($fourCC -eq 'DX10') { throw 'a DX10 DDS header (Iris Studio writes DX9 ones)' }
        if ($levels -le $drop) { throw "only $levels level(s): no smaller one inside" }

        $skip = [int64]0
        for ($level = 0; $level -lt $drop; ++$level) {
            $skip += Get-LevelBytes $fourCC $bits ([Math]::Max(1, $width -shr $level)) ([Math]::Max(1, $height -shr $level))
        }
        $newWidth = [Math]::Max(1, $width -shr $drop)
        $newHeight = [Math]::Max(1, $height -shr $drop)
        [BitConverter]::GetBytes([uint32]$newHeight).CopyTo($header, 12)
        [BitConverter]::GetBytes([uint32]$newWidth).CopyTo($header, 16)
        # the pitch (32-bit) or the first level's bytes (DXT)
        $first = if ($fourCC -eq '') { $newWidth * ($bits / 8) } else { Get-LevelBytes $fourCC $bits $newWidth $newHeight }
        [BitConverter]::GetBytes([uint32]$first).CopyTo($header, 20)
        [BitConverter]::GetBytes([uint32]($levels - $drop)).CopyTo($header, 28)

        $out = [IO.File]::Create($destination)
        try {
            $out.Write($header, 0, 128)
            [void]$in.Seek(128 + $skip, [IO.SeekOrigin]::Begin)
            $in.CopyTo($out, 1MB)
            $bytes = $out.Length
        }
        finally { $out.Dispose() }
        return @($newWidth, $newHeight, $bytes)
    }
    finally { $in.Dispose() }
}

$started = Get-Date
$names = @($index.images.PSObject.Properties.Name)
$halved = 0; $copied = 0; $missing = 0; $failed = 0
$bytesBefore = [int64]0; $bytesAfter = [int64]0
for ($i = 0; $i -lt $names.Count; ++$i) {
    $name = $names[$i]
    $image = $index.images.$name
    if ($i % 200 -eq 0) {
        Write-Progress -Activity 'Making the 2x pack' -Status "$i / $($names.Count): $name" `
            -PercentComplete ([int](100 * $i / [Math]::Max(1, $names.Count)))
    }
    $source = Join-Path $From ($name + '.dds')
    $output = Join-Path $To ($name + '.dds')
    if (-not (Test-Path -LiteralPath $source)) { ++$missing; continue }
    $bytesBefore += (Get-Item -LiteralPath $source).Length

    # the game's texture's size, and its scale in the 2x pack
    $scale = [int]$image.scale
    $gameWidth = [int64]$image.width / $scale
    $gameHeight = [int64]$image.height / $scale
    $wanted = Get-Scale $gameWidth $gameHeight $TargetScale
    $drop = 0
    for ($s = $scale; $s -gt $wanted; $s = $s / 2) { ++$drop }
    try {
        if ($drop -eq 0) {
            Copy-Item -LiteralPath $source -Destination $output -Force
            $size = @([int64]$image.width, [int64]$image.height, (Get-Item -LiteralPath $output).Length)
            ++$copied
        }
        else {
            $size = Copy-FromLevel $source $output $drop
            ++$halved
        }
    }
    catch {
        Write-Warning "${name}: $($_.Exception.Message); left out"
        ++$failed
        $index.images.PSObject.Properties.Remove($name)
        continue
    }
    $image.scale = [Math]::Min($scale, $wanted)
    $image.width = $size[0]
    $image.height = $size[1]
    $image.bytes = $size[2]
    $bytesAfter += $size[2]
}
Write-Progress -Activity 'Making the 2x pack' -Completed
foreach ($name in $names) {
    if ($index.images.PSObject.Properties[$name] -and -not (Test-Path -LiteralPath (Join-Path $To ($name + '.dds')))) {
        $index.images.PSObject.Properties.Remove($name)
    }
}

# each map's scale, images and bytes again (TextureLogic::CountParts: a shared image counts in every map it is in)
foreach ($zone in @($index.parts.PSObject.Properties.Name)) {
    $count = 0; $sum = [int64]0
    foreach ($property in $index.images.PSObject.Properties) {
        if (@($property.Value.parts) -contains $zone) { ++$count; $sum += [int64]$property.Value.bytes }
    }
    $part = $index.parts.$zone
    $part.scale = [Math]::Min([int]$part.scale, $TargetScale)
    $part.images = $count
    $part.bytes = $sum
}
# (UTF-8 without a byte order mark, as Iris Studio writes it)
[IO.File]::WriteAllText((Join-Path $To 'index.json'), ($index | ConvertTo-Json -Depth 10), (New-Object Text.UTF8Encoding $false))

$elapsed = (Get-Date) - $started
'{0} textures halved, {1} copied as they were (already 2x or below), {2} missing, {3} failed' -f $halved, $copied, $missing, $failed
'{0:N2} GB to {1:N2} GB in {2:N0} s, in {3}' -f ($bytesBefore / 1GB), ($bytesAfter / 1GB), $elapsed.TotalSeconds, $To
