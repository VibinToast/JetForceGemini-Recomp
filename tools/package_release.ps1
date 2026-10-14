# Makes the release build and dist\JFG-<version>-windows-x64-portable.zip, and uploads the zip to the GitHub release v<version>
# if the GitHub CLI (gh) is installed and signed in, together with a .sha256 file the launcher's updater checks the download
# against. Run it through package_release.bat. It:
#   1. asks which version this is (Enter keeps the one in version.txt) and writes it to version.txt,
#   2. builds the game with build_windows.bat, so the launcher shows that version,
#   3. packs the zip from the bin folder and uploads it.
#   package_release.bat              asks for the version
#   package_release.bat 1.0.1        uses that version without asking
#   package_release.bat 1.0.1 -SkipBuild   packs what is already in bin (no new build)
param([string]$Version = "", [switch]$SkipBuild)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$versionFile = Join-Path $root "version.txt"
$current = if (Test-Path $versionFile) { (Get-Content $versionFile -Raw).Trim() } else { "" }
if (-not $Version) {
    $answer = Read-Host "Version to release (three numbers, Enter = $current)"
    $Version = if ($answer.Trim()) { $answer.Trim() } else { $current }
}
$Version = $Version.TrimStart("v")
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "The version must be three numbers like 1.0.0, not '$Version'." }
if ($Version -ne $current) {
    [System.IO.File]::WriteAllText($versionFile, "$Version`n")
    Write-Host "version.txt: $current -> $Version"
}
$bin = Join-Path $root "bin"
if (-not $SkipBuild) {
    Write-Host "Building $Version with build_windows.bat (this takes a while)..."
    $started = Get-Date
    $env:JFG_NO_PAUSE = "1"
    & cmd.exe /c ('"' + (Join-Path $root "build_windows.bat") + '"')
    Remove-Item Env:JFG_NO_PAUSE -ErrorAction SilentlyContinue
    $exe = Join-Path $bin "JetForceGeminiRecompiled.exe"
    if (-not (Test-Path $exe) -or (Get-Item $exe).LastWriteTime -lt $started) { throw "The build did not produce a new bin\JetForceGeminiRecompiled.exe. See build_windows.log." }
}
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

# The launcher's updater only installs a download whose SHA-256 matches this file, so it is uploaded with the zip.
$hash = (Get-FileHash $zipPath -Algorithm SHA256).Hash.ToLower()
$sumPath = "$zipPath.sha256"
[System.IO.File]::WriteAllText($sumPath, "$hash  $name.zip`n")
Write-Host "SHA-256: $hash"

if (Get-Command gh -ErrorAction SilentlyContinue) {
    Push-Location $root
    try {
        gh release view "v$Version" *> $null
        if ($LASTEXITCODE -eq 0) {
            gh release upload "v$Version" $zipPath $sumPath --clobber
            if ($LASTEXITCODE -eq 0) { Write-Host "Uploaded to the GitHub release v$Version." }
        } else {
            Write-Host "No GitHub release v$Version yet. Run the Release workflow on GitHub first, then run this again."
        }
    } finally { Pop-Location }
} else {
    Write-Host "gh (GitHub CLI) is not installed. Add $name.zip to the v$Version release on GitHub by hand."
}
