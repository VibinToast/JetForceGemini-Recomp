# Splits a large texture pack (.rtz) into parts small enough for a GitHub release (under 2 GB each): 2 parts, or more
# if 2 would still be too big. Used by split_rtz.bat: drag a .rtz onto the .bat. The pack you drag is not changed.
#
# Result, next to the pack: <pack>.rtz.001, <pack>.rtz.002, ... and <pack>.rtz.sha256 (the SHA-256 of the whole pack
# and of each part). Joined back in order they make the pack again: copy /b <pack>.rtz.001 + <pack>.rtz.002 <pack>.rtz
param([string]$Pack)

$ErrorActionPreference = 'Stop'
$Parts = 2
$MaxPart = 2GB - 1MB # GitHub's limit is 2 GiB per file

function Done([int]$code) {
    Write-Host ''
    Read-Host 'Press Enter to close' | Out-Null
    exit $code
}

if ([string]::IsNullOrWhiteSpace($Pack) -or -not (Test-Path -LiteralPath $Pack -PathType Leaf) -or
    [System.IO.Path]::GetExtension($Pack) -ne '.rtz') {
    Write-Host 'Drag a texture pack .rtz file onto split_rtz.bat.'
    Done 1
}
$src = (Resolve-Path -LiteralPath $Pack).Path
$length = (Get-Item -LiteralPath $src).Length
while ([math]::Ceiling($length / $Parts) -gt $MaxPart) { $Parts++ }
$partSize = [long][math]::Ceiling($length / $Parts)

Write-Host ('Pack    : ' + $src + '  (' + [math]::Round($length / 1MB, 1) + ' MB)')
Write-Host ('Parts   : ' + $Parts + ' of up to ' + [math]::Round($partSize / 1MB, 1) + ' MB')
Write-Host ''

$names = @()
for ($i = 1; $i -le $Parts; $i++) { $names += ($src + '.' + $i.ToString('000')) }
$existing = @($names | Where-Object { Test-Path -LiteralPath $_ })
if ($existing.Count -gt 0) {
    $answer = Read-Host 'Parts from an earlier split are there. Replace them? (y/n)'
    if ($answer -notmatch '^[yY]') { Done 1 }
}
# Parts left over from an earlier split into more pieces would be joined by mistake: remove them.
for ($i = $Parts + 1; Test-Path -LiteralPath ($src + '.' + $i.ToString('000')); $i++) {
    Remove-Item -LiteralPath ($src + '.' + $i.ToString('000')) -Force
}

$buffer = New-Object byte[] (8MB)
$reader = [System.IO.File]::OpenRead($src)
try {
    for ($i = 0; $i -lt $Parts; $i++) {
        $left = [math]::Min($partSize, $length - $reader.Position)
        Write-Host ('Writing ' + (Split-Path -Path $names[$i] -Leaf) + '...')
        $output = [System.IO.File]::Create($names[$i])
        try {
            while ($left -gt 0) {
                $got = $reader.Read($buffer, 0, [int][math]::Min($buffer.Length, $left))
                if ($got -le 0) { throw 'The pack ended early.' }
                $output.Write($buffer, 0, $got)
                $left -= $got
            }
        } finally { $output.Dispose() }
    }
} finally { $reader.Dispose() }

Write-Host ''
Write-Host 'Working out the SHA-256 checksums...'
$lines = @()
$hash = (Get-FileHash -LiteralPath $src -Algorithm SHA256).Hash.ToLowerInvariant()
$lines += ($hash + '  ' + (Split-Path -Path $src -Leaf))
foreach ($name in $names) {
    $lines += ((Get-FileHash -LiteralPath $name -Algorithm SHA256).Hash.ToLowerInvariant() + '  ' + (Split-Path -Path $name -Leaf))
}
$shaFile = $src + '.sha256'
[System.IO.File]::WriteAllLines($shaFile, $lines)

Write-Host ''
foreach ($name in $names) { Write-Host ('Done: ' + $name + '  (' + [math]::Round((Get-Item -LiteralPath $name).Length / 1MB, 1) + ' MB)') }
Write-Host ('Checksums: ' + $shaFile)
$lines | ForEach-Object { Write-Host ('  ' + $_) }
Done 0
