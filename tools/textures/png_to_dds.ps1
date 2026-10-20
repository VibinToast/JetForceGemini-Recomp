# Converts every PNG in a texture pack folder to DDS (BC7, with mipmaps) using texconv.exe.
# Used by png_to_dds.bat: drag a pack folder onto the .bat. Nothing in the original folder is changed.
#
# Result: a new folder next to the original, named <folder>_dds, holding the .dds files and a copy of rt64.json
# with its .png paths changed to .dds.
#
# texconv.exe (Microsoft DirectXTex, https://github.com/microsoft/DirectXTex/releases) is looked for next to this
# script first, then on PATH.
param([string]$Folder)

# ---- Settings ---------------------------------------------------------------------------------------------------
# BC7_UNORM: best quality, handles alpha. BC1_UNORM is smaller but lower quality and has no real alpha.
$Format = 'BC7_UNORM'
# -srgb makes texconv keep the PNG's raw values instead of converting them from sRGB to linear (the game reads PNGs
# as plain 8-bit values). If the DDS textures look darker or lighter than the PNG ones in game, set this to $false.
$KeepRawValues = $true
# -----------------------------------------------------------------------------------------------------------------

$ErrorActionPreference = 'Stop'

function Done([int]$code) {
    Write-Host ''
    Read-Host 'Press Enter to close' | Out-Null
    exit $code
}

if ([string]::IsNullOrWhiteSpace($Folder) -or -not (Test-Path -LiteralPath $Folder -PathType Container)) {
    Write-Host 'Drag a texture pack folder onto png_to_dds.bat.'
    Done 1
}
$src = (Resolve-Path -LiteralPath $Folder).Path.TrimEnd('\')

$texconv = $null
$local = Join-Path $PSScriptRoot 'texconv.exe'
if (Test-Path -LiteralPath $local) {
    $texconv = $local
} else {
    $found = Get-Command 'texconv.exe' -ErrorAction SilentlyContinue
    if ($found) { $texconv = $found.Source }
}
if (-not $texconv) {
    Write-Host 'texconv.exe was not found.'
    Write-Host 'Download it from https://github.com/microsoft/DirectXTex/releases and put it next to this script'
    Write-Host ('(' + $PSScriptRoot + ') or anywhere on your PATH.')
    Done 1
}

$pngCount = @(Get-ChildItem -LiteralPath $src -Filter '*.png' -File).Count
if ($pngCount -eq 0) {
    Write-Host ('No .png files in ' + $src)
    Done 1
}

$parent = Split-Path -Path $src -Parent
$name = Split-Path -Path $src -Leaf
$out = Join-Path $parent ($name + '_dds')
New-Item -ItemType Directory -Force -Path $out | Out-Null

Write-Host ('Source : ' + $src + '  (' + $pngCount + ' PNG files)')
Write-Host ('Output : ' + $out)
Write-Host ('Format : ' + $Format + ', full mipmaps' + $(if ($KeepRawValues) { ', raw values kept (-srgb)' } else { '' }))
Write-Host ''

$texArgs = @('-nologo', '-f', $Format, '-m', '0', '-y', '-o', $out)
if ($KeepRawValues) { $texArgs += '-srgb' }
$texArgs += '*.png'

Push-Location -LiteralPath $src
try {
    & $texconv @texArgs
    $code = $LASTEXITCODE
} finally {
    Pop-Location
}

# rt64.json: same list, pointing at the .dds files.
$json = Join-Path $src 'rt64.json'
if (Test-Path -LiteralPath $json) {
    $text = [System.IO.File]::ReadAllText($json)
    $text = $text -replace '\.png"', '.dds"'
    [System.IO.File]::WriteAllText((Join-Path $out 'rt64.json'), $text, (New-Object System.Text.UTF8Encoding($false)))
    Write-Host 'Wrote rt64.json with .dds paths.'
} else {
    Write-Host 'No rt64.json in the source folder. Copy or generate one before packing (the game writes it for folder packs).'
}

# The pack's details: credits.json and the image it names (pack_image.png unless it says otherwise), copied as they are.
$creditsJson = Join-Path $src 'credits.json'
if (Test-Path -LiteralPath $creditsJson) {
    Copy-Item -LiteralPath $creditsJson -Destination $out -Force
    $image = 'pack_image.png'
    try { $info = Get-Content -LiteralPath $creditsJson -Raw | ConvertFrom-Json; if ($info.image) { $image = $info.image } } catch { }
    $imagePath = Join-Path $src $image
    if (Test-Path -LiteralPath $imagePath) {
        Copy-Item -LiteralPath $imagePath -Destination (Join-Path $out $image) -Force
        $converted = Join-Path $out ([System.IO.Path]::ChangeExtension($image, '.dds'))
        if (Test-Path -LiteralPath $converted) { Remove-Item -LiteralPath $converted -Force }
        $pngCount--
    }
    Write-Host 'Copied credits.json and the pack image.'
}

$ddsCount = @(Get-ChildItem -LiteralPath $out -Filter '*.dds' -File).Count
Write-Host ''
Write-Host ($ddsCount.ToString() + ' of ' + $pngCount + ' textures converted.')
if ($code -ne 0 -or $ddsCount -lt $pngCount) {
    Write-Host 'Some files failed. Scroll up for the texconv messages.'
    Done 1
}
Write-Host 'Done. To make a pack file, drag the output folder onto make_rtz.bat.'
Done 0
