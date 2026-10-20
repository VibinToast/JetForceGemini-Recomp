# Makes a half-resolution copy of a packed texture pack (.rtz): 4K -> 2K, then run it on the 2K for the 1K.
# Used by halve_rtz.bat: drag a .rtz onto the .bat. The pack you drag is not changed.
#
# - DDS textures with mipmaps lose their largest level: the next level down is the same texture at half size, so
#   nothing is recompressed (fast, and no extra quality loss).
# - PNG textures are scaled to half size.
# - Only files named by a texture hash are halved. credits.json, pack_image.png, rt64.json and anything else are copied
#   as they are, apart from credits.json's "variant", which you are asked for (e.g. 2K).
#
# Result, next to the pack you dragged: <id>-<variant>_v<version>.rtz, from credits.json's "id" and "version" and the
# variant you give, e.g. Official_Recomp_HD-2K_v1.1.0.rtz. The game shows it as Official_Recomp_HD-2K.
param([string]$Pack)

$ErrorActionPreference = 'Stop'

function Done([int]$code) {
    Write-Host ''
    Read-Host 'Press Enter to close' | Out-Null
    exit $code
}

if ([string]::IsNullOrWhiteSpace($Pack) -or -not (Test-Path -LiteralPath $Pack -PathType Leaf) -or
    [System.IO.Path]::GetExtension($Pack) -ne '.rtz') {
    Write-Host 'Drag a texture pack .rtz file onto halve_rtz.bat.'
    Done 1
}
$src = (Resolve-Path -LiteralPath $Pack).Path

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName System.Drawing

$in = [System.IO.Compression.ZipFile]::OpenRead($src)
try {
    # ---- The variant and the new file ---------------------------------------------------------------------------
    $credits = $null
    $creditsEntry = $in.GetEntry('credits.json')
    if ($creditsEntry -ne $null) {
        $reader = New-Object System.IO.StreamReader($creditsEntry.Open())
        try { $credits = $reader.ReadToEnd() | ConvertFrom-Json } finally { $reader.Dispose() }
    }
    Write-Host ('Pack    : ' + $src)
    if ($credits -eq $null) {
        Write-Host 'The pack has no credits.json. Add one with its id and version (see docs\TEXTURE_PACKS.md).'
        Done 1
    }
    function Field([string]$key) { if ($credits.PSObject.Properties[$key]) { ([string]$credits.$key).Trim() } else { '' } }
    $id = Field 'id'
    $version = Field 'version'
    $oldVariant = Field 'variant'
    Write-Host ('Id      : ' + $(if ($id) { $id } else { '(none)' }))
    Write-Host ('Variant : ' + $(if ($oldVariant) { $oldVariant } else { '(none)' }))
    Write-Host ('Version : ' + $(if ($version) { $version } else { '(none)' }))
    while (-not $id) {
        $id = (Read-Host 'The pack has no id. Id for the new pack (e.g. Official_Recomp_HD)').Trim()
    }
    $variant = ''
    while (-not $variant) {
        $variant = (Read-Host 'Variant of the half-size pack (e.g. 2K)').Trim()
    }
    $fileName = $id + '-' + $variant
    if ($version) { $fileName += '_v' + ($version -replace '^[vV]', '') }
    $fileName += '.rtz'
    if ($fileName.IndexOfAny([System.IO.Path]::GetInvalidFileNameChars()) -ge 0) {
        Write-Host ('"' + $fileName + '" cannot be used as a file name.')
        Done 1
    }
    $out = Join-Path (Split-Path -Path $src -Parent) $fileName
    if ($out -eq $src) {
        Write-Host 'That is the name of the pack you dragged: give a different variant.'
        Done 1
    }
    if (Test-Path -LiteralPath $out) {
        $overwrite = Read-Host ((Split-Path -Path $out -Leaf) + ' is already there. Replace it? (y/n)')
        if ($overwrite -notmatch '^[yY]') { Done 1 }
        Remove-Item -LiteralPath $out -Force
    }
    Write-Host ('Into    : ' + $out)
    Write-Host ''

    # ---- Copy, halving the textures -----------------------------------------------------------------------------
    $zip = [System.IO.Compression.ZipFile]::Open($out, [System.IO.Compression.ZipArchiveMode]::Create)
    $halved = 0; $kept = 0; $skipped = New-Object System.Collections.Generic.List[string]
    $n = 0; $total = $in.Entries.Count
    try {
        foreach ($entry in $in.Entries) {
            $n++
            if ($n % 500 -eq 0) { Write-Host ('  ' + $n + ' / ' + $total) }
            if ($entry.FullName.EndsWith('/')) { continue } # a folder
            $name = $entry.Name
            $isTexture = $name -match '^[0-9a-fA-F]{16}'
            $ext = [System.IO.Path]::GetExtension($name).ToLowerInvariant()

            $stream = $entry.Open()
            $memory = New-Object System.IO.MemoryStream([int]$entry.Length)
            try { $stream.CopyTo($memory) } finally { $stream.Dispose() }
            $bytes = $memory.ToArray()
            $memory.Dispose()

            if ($entry.FullName -eq 'credits.json') {
                if ($credits.PSObject.Properties['id']) { $credits.id = $id }
                else { $credits | Add-Member -NotePropertyName id -NotePropertyValue $id }
                if ($credits.PSObject.Properties['variant']) { $credits.variant = $variant }
                else { $credits | Add-Member -NotePropertyName variant -NotePropertyValue $variant }
                # Written field by field, in the file's own order, two spaces in (Windows PowerShell's ConvertTo-Json
                # pads the colons and writes ' & < > as \u escapes, which are put back).
                $fields = foreach ($p in $credits.PSObject.Properties) {
                    $value = (ConvertTo-Json -InputObject $p.Value -Compress -Depth 5) -replace '\\u0027', "'" -replace '\\u0026', '&' -replace '\\u003c', '<' -replace '\\u003e', '>'
                    '  "' + $p.Name + '": ' + $value
                }
                $text = "{`r`n" + ($fields -join ",`r`n") + "`r`n}`r`n"
                $bytes = (New-Object System.Text.UTF8Encoding($false)).GetBytes($text)
            }
            elseif ($isTexture -and $ext -eq '.dds') {
                $result = $null
                # DDS: "DDS " + 124-byte header (+ 20-byte DX10 header). Height at 12, width at 16, linear size at 20,
                # mip count at 28, pixel format's FourCC at 84; DX10's DXGI format at 128.
                if ($bytes.Length -ge 128 -and [System.Text.Encoding]::ASCII.GetString($bytes, 0, 4) -eq 'DDS ') {
                    $height = [BitConverter]::ToUInt32($bytes, 12)
                    $width = [BitConverter]::ToUInt32($bytes, 16)
                    $mips = [BitConverter]::ToUInt32($bytes, 28)
                    $fourcc = [System.Text.Encoding]::ASCII.GetString($bytes, 84, 4)
                    $dataStart = 128
                    $blockBytes = 0
                    if ($fourcc -eq 'DX10' -and $bytes.Length -ge 148) {
                        $dataStart = 148
                        $dxgi = [BitConverter]::ToUInt32($bytes, 128)
                        # BC1 / BC4: 8 bytes per 4x4 block; BC2, BC3, BC5, BC6H, BC7: 16.
                        if ($dxgi -in 70, 71, 72, 79, 80, 81) { $blockBytes = 8 }
                        elseif ($dxgi -in 73, 74, 75, 76, 77, 78, 82, 83, 84, 94, 95, 96, 97, 98, 99) { $blockBytes = 16 }
                    }
                    elseif ($fourcc -in 'DXT1', 'ATI1', 'BC4U') { $blockBytes = 8 }
                    elseif ($fourcc -in 'DXT3', 'DXT5', 'ATI2', 'BC5U') { $blockBytes = 16 }
                    if ($blockBytes -gt 0 -and $mips -ge 2) {
                        # The size of a mip level: whole 4x4 blocks.
                        $top = [int]([math]::Max(1, [math]::Ceiling($width / 4)) * [math]::Max(1, [math]::Ceiling($height / 4)) * $blockBytes)
                        $newWidth = [int][math]::Max(1, [math]::Floor($width / 2))
                        $newHeight = [int][math]::Max(1, [math]::Floor($height / 2))
                        $newTop = [int]([math]::Max(1, [math]::Ceiling($newWidth / 4)) * [math]::Max(1, [math]::Ceiling($newHeight / 4)) * $blockBytes)
                        if ($bytes.Length -ge $dataStart + $top + $newTop) {
                            $result = New-Object byte[] ([int]($bytes.Length - $top))
                            [Array]::Copy($bytes, 0, $result, 0, $dataStart)
                            [Array]::Copy($bytes, $dataStart + $top, $result, $dataStart, $bytes.Length - $dataStart - $top)
                            [Array]::Copy([BitConverter]::GetBytes([uint32]$newHeight), 0, $result, 12, 4)
                            [Array]::Copy([BitConverter]::GetBytes([uint32]$newWidth), 0, $result, 16, 4)
                            [Array]::Copy([BitConverter]::GetBytes([uint32]$newTop), 0, $result, 20, 4)
                            [Array]::Copy([BitConverter]::GetBytes([uint32]($mips - 1)), 0, $result, 28, 4)
                        }
                    }
                }
                if ($result -ne $null) { $bytes = $result; $halved++ } else { $skipped.Add($entry.FullName); $kept++ }
            }
            elseif ($isTexture -and $ext -eq '.png') {
                $image = [System.Drawing.Image]::FromStream((New-Object System.IO.MemoryStream(, $bytes)))
                try {
                    $newWidth = [math]::Max(1, [math]::Floor($image.Width / 2))
                    $newHeight = [math]::Max(1, [math]::Floor($image.Height / 2))
                    $small = New-Object System.Drawing.Bitmap($newWidth, $newHeight, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
                    $g = [System.Drawing.Graphics]::FromImage($small)
                    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
                    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
                    $g.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
                    $g.DrawImage($image, 0, 0, $newWidth, $newHeight)
                    $g.Dispose()
                    $png = New-Object System.IO.MemoryStream
                    $small.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
                    $small.Dispose()
                    $bytes = $png.ToArray()
                    $png.Dispose()
                    $halved++
                } finally { $image.Dispose() }
            }
            elseif ($isTexture) { $skipped.Add($entry.FullName); $kept++ }

            $outEntry = $zip.CreateEntry($entry.FullName, [System.IO.Compression.CompressionLevel]::NoCompression)
            $outEntry.LastWriteTime = $entry.LastWriteTime
            $write = $outEntry.Open()
            try { $write.Write($bytes, 0, $bytes.Length) } finally { $write.Dispose() }
        }
    } finally {
        $zip.Dispose()
    }
} finally {
    $in.Dispose()
}

$mb = [math]::Round((Get-Item -LiteralPath $out).Length / 1MB, 1)
Write-Host ''
Write-Host ('Done: ' + $out + '  (' + $mb + ' MB)')
Write-Host ($halved.ToString() + ' textures halved.')
if ($kept -gt 0) {
    Write-Host ($kept.ToString() + ' textures copied at their own size (no mipmaps, or a format this script does not know):')
    $skipped | Select-Object -First 20 | ForEach-Object { Write-Host ('  ' + $_) }
    if ($kept -gt 20) { Write-Host '  ...' }
}
Write-Host ('Its credits.json: id ' + $id + ', variant ' + $variant + '.')
Done 0
