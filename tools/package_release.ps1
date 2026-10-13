# Makes dist\JFG-<version>-windows-x64-portable.zip from the bin folder, and uploads it to the GitHub release v<version>
# if the GitHub CLI (gh) is installed and signed in. Run it through package_release.bat.
#   package_release.bat            uses the number in version.txt
#   package_release.bat 1.0.0      uses the number you give
param([string]$Version = "")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $Version) { $Version = (Get-Content (Join-Path $root "version.txt") -Raw).Trim() }
$Version = $Version.TrimStart("v")
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "The version must be three numbers like 1.0.0, not '$Version'." }
$bin = Join-Path $root "bin"
if (-not (Test-Path (Join-Path $bin "JetForceGeminiRecompiled.exe"))) { throw "bin\JetForceGeminiRecompiled.exe not found. Run build_windows.bat first." }

$name = "JFG-$Version-windows-x64-portable"
$dist = Join-Path $root "dist"
New-Item -ItemType Directory -Force $dist | Out-Null
$zipPath = Join-Path $dist "$name.zip"
if (Test-Path $zipPath) { Remove-Item $zipPath }

# Only what a new player needs. Saves, settings, dumps and ROM copies in bin are yours and stay out.
$files = @{}
Get-ChildItem $bin -File | Where-Object { $_.Extension -in ".exe", ".dll" } | ForEach-Object { $files[$_.Name] = $_.FullName }
$files["recompcontrollerdb.txt"] = Join-Path $bin "recompcontrollerdb.txt"
Get-ChildItem (Join-Path $bin "assets") -File -Recurse | ForEach-Object {
    $files[$_.FullName.Substring($bin.Length + 1).Replace("\", "/")] = $_.FullName
}
foreach ($f in "LICENSE", "README.md", "CHANGELOG.md") { $files[$f] = Join-Path $root $f }
$files["docs/TEXTURE_PACKS.md"] = Join-Path $root "docs\TEXTURE_PACKS.md"

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::Open($zipPath, "Create")
try {
    foreach ($key in ($files.Keys | Sort-Object)) {
        if (-not (Test-Path $files[$key])) { throw "Missing file: $($files[$key])" }
        [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $files[$key], "$name/$($key.Replace('\', '/'))", "Optimal")
    }
    foreach ($d in "mods", "texture_packs") { [void]$zip.CreateEntry("$name/$d/") }   # empty folders
} finally { $zip.Dispose() }
Write-Host "Made $zipPath ($([math]::Round((Get-Item $zipPath).Length / 1MB, 1)) MB)"

if (Get-Command gh -ErrorAction SilentlyContinue) {
    Push-Location $root
    try {
        gh release view "v$Version" *> $null
        if ($LASTEXITCODE -eq 0) {
            gh release upload "v$Version" $zipPath --clobber
            if ($LASTEXITCODE -eq 0) { Write-Host "Uploaded to the GitHub release v$Version." }
        } else {
            Write-Host "No GitHub release v$Version yet. Run the Release workflow on GitHub first, then run this again."
        }
    } finally { Pop-Location }
} else {
    Write-Host "gh (GitHub CLI) is not installed. Add $name.zip to the v$Version release on GitHub by hand."
}
