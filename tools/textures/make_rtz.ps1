# Packs a texture pack folder into a .rtz file (a zip with rt64.json at its root) for RT64 / the game.
# Used by make_rtz.bat: drag a pack folder onto the .bat. Nothing in the folder is changed.
#
# Result: <folder>.rtz next to the folder. Put it in bin\texture_packs\ (chosen in the Textures tab) or in bin\mods\
# (loaded as a mod, switched in the Mods tab).
param([string]$Folder)

# ---- Settings ---------------------------------------------------------------------------------------------------
# 'NoCompression' (stored): fastest to load, best for PNG (already compressed).
# 'Optimal': smaller file, but each texture has to be unpacked when it is loaded. May help for DDS.
$Compression = 'NoCompression'
# -----------------------------------------------------------------------------------------------------------------

$ErrorActionPreference = 'Stop'

function Done([int]$code) {
    Write-Host ''
    Read-Host 'Press Enter to close' | Out-Null
    exit $code
}

if ([string]::IsNullOrWhiteSpace($Folder) -or -not (Test-Path -LiteralPath $Folder -PathType Container)) {
    Write-Host 'Drag a texture pack folder onto make_rtz.bat.'
    Done 1
}
$src = (Resolve-Path -LiteralPath $Folder).Path.TrimEnd('\')

if (-not (Test-Path -LiteralPath (Join-Path $src 'rt64.json'))) {
    Write-Host ('There is no rt64.json in ' + $src)
    Write-Host 'A pack needs rt64.json at its top level. Run the game once with the folder pack selected (it writes one), or'
    Write-Host 'convert with png_to_dds.bat, which copies it across.'
    Done 1
}

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$parent = Split-Path -Path $src -Parent
$name = Split-Path -Path $src -Leaf
$out = Join-Path $parent ($name + '.rtz')
if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Force }

$level = [System.IO.Compression.CompressionLevel]::$Compression
$files = @(Get-ChildItem -LiteralPath $src -Recurse -File | Where-Object {
    $_.Name -in 'rt64.json', 'credits.json' -or $_.Extension -in '.dds', '.png'
})
Write-Host ('Packing ' + $files.Count + ' files from ' + $src)
Write-Host ('Into    ' + $out + '  (' + $Compression + ')')

$zip = [System.IO.Compression.ZipFile]::Open($out, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    $n = 0
    foreach ($f in $files) {
        $rel = $f.FullName.Substring($src.Length + 1).Replace('\', '/')
        [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $f.FullName, $rel, $level)
        $n++
        if ($n % 1000 -eq 0) { Write-Host ('  ' + $n + ' / ' + $files.Count) }
    }
} finally {
    $zip.Dispose()
}

$mb = [math]::Round((Get-Item -LiteralPath $out).Length / 1MB, 1)
Write-Host ''
Write-Host ('Done: ' + $out + '  (' + $mb + ' MB)')
Write-Host 'Put it in bin\texture_packs\ or bin\mods\.'
Done 0
