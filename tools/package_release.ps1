# Builds the Windows release and uploads it to GitHub. Run it through package_release.bat. Needs the GitHub CLI (gh),
# signed in (gh auth login), for anything that talks to GitHub. See RELEASING.md in the owner's Recomp-Docs notes.
#
#   package_release.bat                    finds the draft release waiting for a Windows build (made by the Release workflow
#                                          when something is merged into main), builds that release's exact commit in
#                                          dist\src-vX.Y.Z (your own checkout, changes and bin folder are not touched),
#                                          uploads JFG-X.Y.Z-windows-x64-portable.zip and its .sha256, and publishes the
#                                          release once the Linux files are there too
#   package_release.bat -Publish           the same, but publishes even when the Linux files are not there yet
#   package_release.bat 1.0.1              builds this checkout as 1.0.1 (writes version.txt) and uploads to v1.0.1 if
#                                          that release is a draft (a published release is never changed)
#   package_release.bat 1.0.1 -SkipBuild   packs what is already in bin (no new build)
#
# The .sha256 file is what the launcher's updater checks a download against. Saves, settings, dumps and ROM copies in bin
# never go into the zip.
param([string]$Version = "", [switch]$SkipBuild, [switch]$Publish)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$rom = if ($env:JFG_ROM) { $env:JFG_ROM } else { Join-Path $root "baserom.us.z64" }

# Every file a release needs before it is published. Keep in step with tools/package_release.sh.
function Release-Files([string]$v) {
    "JFG-$v-windows-x64-portable.zip", "JFG-$v-windows-x64-portable.zip.sha256",
    "JFG-$v-linux-x86_64.AppImage", "JFG-$v-linux-x86_64.AppImage.sha256",
    "JFG-$v-linux-x86_64-portable.tar.gz", "JFG-$v-linux-x86_64-portable.tar.gz.sha256"
}
function Gh-Json([string[]]$ghArgs) {
    $out = & gh @ghArgs
    if ($LASTEXITCODE -ne 0) { throw "gh $($ghArgs -join ' ') failed." }
    ($out -join "`n") | ConvertFrom-Json
}
function Have-Gh { [bool](Get-Command gh -ErrorAction SilentlyContinue) }
# Runs a command with its output thrown away and returns its exit code. (Windows PowerShell turns a redirected error
# message from a program into a script error under ErrorActionPreference Stop, so it is relaxed here.)
function Quiet([scriptblock]$block) {
    $old = $ErrorActionPreference; $ErrorActionPreference = "Continue"
    try { & $block *> $null } finally { $ErrorActionPreference = $old }
    $LASTEXITCODE
}

$src = $root          # the checkout that is built and packed
$tag = ""             # the GitHub release the files go to
$worktree = ""
Push-Location $root
try {
    if (-not $Version) {
        # ---- find the draft ----------------------------------------------------------------------------------------
        if (-not (Have-Gh)) { throw "gh (GitHub CLI) is not installed: https://cli.github.com, then run gh auth login." }
        if ((Quiet { gh auth status }) -ne 0) { throw "gh is not signed in. Run gh auth login first." }
        $drafts = @(Gh-Json @("release", "list", "--limit", "50", "--json", "tagName,isDraft") | Where-Object { $_.isDraft })
        foreach ($d in $drafts) {
            $info = Gh-Json @("release", "view", $d.tagName, "--json", "tagName,assets,targetCommitish")
            $v = $info.tagName.TrimStart("v")
            if (@($info.assets | ForEach-Object { $_.name }) -notcontains "JFG-$v-windows-x64-portable.zip") { $tag = $info.tagName; $commit = $info.targetCommitish; break }
        }
        if (-not $tag -and $Publish -and $drafts.Count) {
            & gh release edit $drafts[0].tagName --draft=false
            if ($LASTEXITCODE -ne 0) { throw "Publishing $($drafts[0].tagName) failed." }
            Write-Host "Published $($drafts[0].tagName)."
            exit 0
        }
        if (-not $tag) {
            if ($drafts.Count) { Write-Host "The draft release $($drafts[0].tagName) already has its Windows build." }
            else { Write-Host "No draft release is waiting for a build. Merge into main (or run the Release workflow) to make one." }
            exit 0
        }
        $Version = $tag.TrimStart("v")
        Write-Host "Draft release $tag is waiting for a Windows build (commit $commit)."

        # ---- that release's exact commit, in its own folder ----------------------------------------------------------
        & git fetch -q origin
        if ($LASTEXITCODE -ne 0) { throw "git fetch failed." }
        if ((Quiet { git cat-file -e "$commit^{commit}" }) -ne 0) { throw "Commit $commit of $tag is not on GitHub's main branch. Was it pushed?" }
        $worktree = Join-Path $root "dist\src-$tag"
        if (Test-Path $worktree) { [void](Quiet { git worktree remove --force $worktree }); if (Test-Path $worktree) { Remove-Item -Recurse -Force $worktree } }
        & git worktree prune
        & git worktree add --detach $worktree $commit
        if ($LASTEXITCODE -ne 0) { throw "git worktree add failed." }
        $src = $worktree
        $inTree = (Get-Content (Join-Path $src "version.txt") -Raw).Trim()
        if ($inTree -ne $Version) { throw "Commit $commit has version.txt $inTree, not $Version." }
        if ($SkipBuild) { throw "-SkipBuild only works with a version (package_release.bat 1.0.1 -SkipBuild)." }
    } else {
        # ---- this checkout, as the version given ---------------------------------------------------------------------
        $Version = $Version.TrimStart("v")
        # A test build (a GitHub prerelease, offered on the launcher's Testing update channel): 1.4.0-test.4.
        if ($Version -notmatch '^\d+\.\d+\.\d+(-test\.\d+)?$') { throw "The version must be three numbers like 1.0.0 (a test build: 1.4.0-test.1), not '$Version'." }
        $tag = "v$Version"
        $versionFile = Join-Path $root "version.txt"
        $current = if (Test-Path $versionFile) { (Get-Content $versionFile -Raw).Trim() } else { "" }
        if ($Version -ne $current) {
            [System.IO.File]::WriteAllText($versionFile, "$Version`n")
            Write-Host "version.txt: $current -> $Version"
        }
    }

    # ---- build -------------------------------------------------------------------------------------------------------
    $bin = Join-Path $src "bin"
    if (-not $SkipBuild) {
        if (-not (Test-Path $rom)) { throw "ROM not found: $rom (or set JFG_ROM)." }
        Write-Host "Building $Version with build_windows.bat (this takes a while)..."
        $started = Get-Date
        $env:JFG_NO_PAUSE = "1"
        $env:JFG_ROM = $rom
        & cmd.exe /c ('"' + (Join-Path $src "build_windows.bat") + '"')
        Remove-Item Env:JFG_NO_PAUSE -ErrorAction SilentlyContinue
        $exe = Join-Path $bin "JetForceGeminiRecompiled.exe"
        if (-not (Test-Path $exe) -or (Get-Item $exe).LastWriteTime -lt $started) { throw "The build did not produce a new $exe. See $(Join-Path $src 'build_windows.log')." }
    }
    if (-not (Test-Path (Join-Path $bin "JetForceGeminiRecompiled.exe"))) { throw "bin\JetForceGeminiRecompiled.exe not found. Run build_windows.bat first." }

    # ---- zip ---------------------------------------------------------------------------------------------------------
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
    foreach ($f in "LICENSE", "README.md", "CHANGELOG.md") { $files[$f] = Join-Path $src $f }
    $files["docs/TEXTURE_PACKS.md"] = Join-Path $src "docs\TEXTURE_PACKS.md"

    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    # The order matters to the launcher's updater (1.2.0 and older), which installs the files in zip order and, when it
    # cannot replace the running exe, loses the files it had already installed. So the exe comes first, then the DLLs and
    # the controller database, then assets/ and the rest: when the exe is locked, nothing has been installed yet.
    $order = { param($k) if ($k -ieq "JetForceGeminiRecompiled.exe") { 0 } elseif ($k -notmatch "/" -and $k -like "*.dll") { 1 } elseif ($k -ieq "recompcontrollerdb.txt") { 2 } elseif ($k -like "assets/*") { 3 } else { 4 } }
    $zip = [System.IO.Compression.ZipFile]::Open($zipPath, "Create")
    try {
        foreach ($key in ($files.Keys | Sort-Object { & $order $_ }, { $_ })) {
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

    # ---- upload, and publish once every file is there ----------------------------------------------------------------
    if (-not (Have-Gh)) { Write-Host "gh (GitHub CLI) is not installed. Add $name.zip and its .sha256 to the $tag release on GitHub by hand."; exit 0 }
    if ((Quiet { gh release view $tag }) -ne 0) { Write-Host "No GitHub release $tag. Merge into main (or run the Release workflow) to make one, then run this again."; exit 0 }
    # Only a draft gets files: a published release is never changed, so a test build cannot replace what players download.
    if (-not (Gh-Json @("release", "view", $tag, "--json", "isDraft")).isDraft) { Write-Host "$tag is already published, so nothing is uploaded. The zip is in $dist."; exit 0 }
    & gh release upload $tag $zipPath $sumPath --clobber
    if ($LASTEXITCODE -ne 0) { throw "Uploading to $tag failed." }
    Write-Host "Uploaded to the GitHub release $tag."

    # A test build is never published from here: the owner publishes it (as a prerelease) and links it on the issue.
    if ($Version -match '-test\.') { Write-Host "$tag is a test build: it stays a draft for the owner to publish as a prerelease."; exit 0 }
    $info = Gh-Json @("release", "view", $tag, "--json", "isDraft,assets")
    if ($info.isDraft) {
        $have = @($info.assets | ForEach-Object { $_.name })
        $missing = @(Release-Files $Version | Where-Object { $have -notcontains $_ })
        if ($missing.Count -and -not $Publish) {
            Write-Host "$tag stays a draft until these are uploaded too (tools/package_release.sh on Linux):"
            $missing | ForEach-Object { Write-Host "  $_" }
            Write-Host "To publish it without them: package_release.bat -Publish, or gh release edit $tag --draft=false"
        } else {
            & gh release edit $tag --draft=false
            if ($LASTEXITCODE -ne 0) { throw "Publishing $tag failed." }
            Write-Host "Published $tag : $((Gh-Json @("release", "view", $tag, "--json", "url")).url)"
        }
    }
    if ($worktree) { [void](Quiet { git worktree remove --force $worktree }) }
} finally { Pop-Location }
