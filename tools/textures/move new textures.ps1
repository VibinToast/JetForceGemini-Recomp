# Copies every dumped texture that has no upscaled replacement in the pack yet into texture_new, ready to upscale.
# Textures already in texture_new are left alone and not copied again, so it can be run after every play session.
#
#   .\move new textures.ps1                      (pack: bin\texture_packs\16x_LSDIRDAT)
#   .\move new textures.ps1 -Pack 4x_LSDIRDAT    (another folder in bin\texture_packs)
#   .\move new textures.ps1 -Bin D:\JFG\bin      (a bin folder somewhere else)
#
# Lives in tools\textures (bin is build output and not part of the repository); works on ..\..\bin by default.
#
# texture_dump is searched with its sub-folders (the "One folder per level" dump option), and a texture dumped in
# several levels is copied once. Files are matched by their 16-character hash (the start of the name), so the pack
# may use .png or .dds and any sub-folders. Put the upscaled files in the pack folder under the same names; the game
# rebuilds the pack's rt64.json itself.

param(
    [string]$Pack = "16x_LSDIRDAT",
    [string]$Bin = ""
)

# The game's bin folder: by default the one next to this repository's tools folder (tools\textures\..\..\bin).
if (-not $Bin) { $Bin = Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) "bin" }
$Bin = (Resolve-Path -LiteralPath $Bin -ErrorAction SilentlyContinue).Path
if (-not $Bin) { Write-Error "bin folder not found. Build the game first, or pass -Bin <path to the bin folder>."; exit 1 }
$bin  = $Bin
$dump = Join-Path $bin "texture_dump"
$new  = Join-Path $bin "texture_new"
$packDir = if ([System.IO.Path]::IsPathRooted($Pack)) { $Pack } else { Join-Path (Join-Path $bin "texture_packs") $Pack }

if (-not (Test-Path $packDir)) { Write-Error "Pack folder not found: $packDir"; exit 1 }
if (-not (Test-Path $dump))    { Write-Error "Dump folder not found: $dump"; exit 1 }

New-Item -ItemType Directory -Force $new | Out-Null

# Hashes that already have an upscaled file in the pack.
$done = @{}
Get-ChildItem $packDir -Recurse -File -Include *.png,*.dds | Where-Object { $_.Name -match '^[0-9a-fA-F]{16}' } |
    ForEach-Object { $done[$_.Name.Substring(0,16).ToLower()] = $true }

# Hashes already waiting in texture_new from an earlier run (any sub-folder or suffix), so they are not copied again.
$seen = @{}
$waiting = 0
Get-ChildItem $new -Recurse -File -Include *.png,*.dds | Where-Object { $_.Name -match '^[0-9a-fA-F]{16}' } |
    ForEach-Object { $seen[$_.Name.Substring(0,16).ToLower()] = $true; $waiting++ }

# Dumps whose hash is in neither the pack nor texture_new, one per hash.
$todo = Get-ChildItem $dump -Recurse -File -Filter *.png | Where-Object {
    if ($_.Name -notmatch '^[0-9a-fA-F]{16}') { return $false }
    $h = $_.Name.Substring(0,16).ToLower()
    if ($done.ContainsKey($h) -or $seen.ContainsKey($h)) { return $false }
    $seen[$h] = $true
    return $true
}

$todo | ForEach-Object { Copy-Item $_.FullName -Destination (Join-Path $new $_.Name) }
"Pack: $packDir ($($done.Count) textures)"
"$(@($todo).Count) new textures copied to $new ($waiting were already there)"
