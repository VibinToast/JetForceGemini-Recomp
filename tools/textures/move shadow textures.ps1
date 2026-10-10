# Moves the "shadow" textures out of the texture dump and the texture packs, into bin\texture_shadows, to look over
# and then delete.
#
#   .\move shadow textures.ps1              move them
#   .\move shadow textures.ps1 -ListOnly    only count them and write the list
#   .\move shadow textures.ps1 -Bin D:\JFG\bin   a bin folder somewhere else
#
# Lives in tools\textures (bin is build output and not part of the repository); works on ..\..\bin by default.
#
# What they are: Jet Force Gemini draws each character's shadow by rendering the character's silhouette into a
# 64x64 texture every frame. Every pose gives a new image, so the dump fills up with thousands of them, and a
# replacement for one is almost never used again - upscaling them does nothing in game.
#
# How they are found: in texture_dump (with its per-level sub-folders) and texture_new, a dumped texture is a shadow
# when it is 64x64 and every visible pixel is black (the silhouette; its soft edge is only in the alpha). Glows and
# other white or grey alpha textures of the same size are left alone, and so are the hashes in $Keep below. Then every file
# with the same 16-character hash at the start of its name is moved too: from texture_new and from every pack in
# texture_packs (upscaled copies, any suffix, .png or .dds).
#
# Files keep their folder structure under bin\texture_shadows, e.g. texture_shadows\texture_packs\16x_LSDIRDAT\...
# Nothing is deleted. To undo, move the folders back. The list of hashes is written to texture_shadows\shadows.txt.

param(
    [switch]$ListOnly,
    [string]$Bin = ""
)

# Never treated as shadows (checked by hand).
$Keep = @(
    "0d21a6f5ebf3b8e1", "5ee1846a1d6c5ab0", "5ff755f6e2f4934c", "7aabf0a88e7d50a0",
    "7f93f20f19e0a027", "7f545185f84df9b3", "8fcba711eb8fd5fb", "38d40d0fc455c5a1"
)

# The game's bin folder: by default the one next to this repository's tools folder (tools\textures\..\..\bin).
if (-not $Bin) { $Bin = Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) "bin" }
$Bin = (Resolve-Path -LiteralPath $Bin -ErrorAction SilentlyContinue).Path
if (-not $Bin) { Write-Error "bin folder not found. Build the game first, or pass -Bin <path to the bin folder>."; exit 1 }
$bin   = $Bin
$dump  = Join-Path $bin "texture_dump"
$new   = Join-Path $bin "texture_new"
$packs = Join-Path $bin "texture_packs"
$dest  = Join-Path $bin "texture_shadows"

# PNG check in C# (fast, and needs nothing but .NET): the dumps are 8-bit RGBA, not interlaced.
Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.IO.Compression;
public static class JfgShadowCheck {
    static uint BE(byte[] b, int o) { return (uint)(b[o] << 24 | b[o + 1] << 16 | b[o + 2] << 8 | b[o + 3]); }
    public static bool IsShadow(string path) {
        byte[] f = File.ReadAllBytes(path);
        if (f.Length < 33 || f[1] != (byte)'P' || f[2] != (byte)'N' || f[3] != (byte)'G') return false;
        int w = (int)BE(f, 16), h = (int)BE(f, 20);
        if (w != 64 || h != 64 || f[24] != 8 || f[25] != 6 || f[28] != 0) return false; // 64x64, RGBA8, not interlaced
        MemoryStream idat = new MemoryStream();
        int p = 8;
        while (p + 8 <= f.Length) {
            int len = (int)BE(f, p);
            string type = System.Text.Encoding.ASCII.GetString(f, p + 4, 4);
            if (type == "IDAT") idat.Write(f, p + 8, len);
            if (type == "IEND") break;
            p += 12 + len;
        }
        byte[] z = idat.ToArray();
        if (z.Length < 3) return false;
        int stride = w * 4;
        byte[] raw = new byte[(stride + 1) * h];
        using (DeflateStream ds = new DeflateStream(new MemoryStream(z, 2, z.Length - 2), CompressionMode.Decompress)) {
            int got = 0, n;
            while (got < raw.Length && (n = ds.Read(raw, got, raw.Length - got)) > 0) got += n;
            if (got < raw.Length) return false;
        }
        byte[] img = new byte[stride * h];
        for (int y = 0; y < h; y++) {
            int ft = raw[y * (stride + 1)];
            for (int x = 0; x < stride; x++) {
                int a = x >= 4 ? img[y * stride + x - 4] : 0;
                int b = y > 0 ? img[(y - 1) * stride + x] : 0;
                int c = (x >= 4 && y > 0) ? img[(y - 1) * stride + x - 4] : 0;
                int v = raw[y * (stride + 1) + 1 + x];
                switch (ft) {
                    case 1: v += a; break;
                    case 2: v += b; break;
                    case 3: v += (a + b) / 2; break;
                    case 4: { int pp = a + b - c, pa = Math.Abs(pp - a), pb = Math.Abs(pp - b), pc = Math.Abs(pp - c);
                              v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c); break; }
                }
                img[y * stride + x] = (byte)v;
            }
        }
        int visible = 0;
        for (int i = 0; i < img.Length; i += 4) {
            if (img[i + 3] == 0) continue;
            if (img[i] > 8 || img[i + 1] > 8 || img[i + 2] > 8) return false;  // not black
            visible++;
        }
        return visible > 0;
    }
}
"@

if (-not (Test-Path $dump)) { Write-Error "Dump folder not found: $dump"; exit 1 }

# 1. Find the shadows among the dumps.
$sources = @($dump) + @(if (Test-Path $new) { $new })
$files = foreach ($s in $sources) { Get-ChildItem $s -Recurse -File -Filter *.png }
$shadow = @{}
$i = 0
foreach ($f in $files) {
    $i++
    if ($i % 500 -eq 0) { Write-Progress -Activity "Checking dumped textures" -Status "$i / $($files.Count)" -PercentComplete (100 * $i / $files.Count) }
    if ($f.Name -notmatch '^[0-9a-fA-F]{16}') { continue }
    if ($Keep -contains $f.Name.Substring(0,16).ToLower()) { continue }
    try { if ([JfgShadowCheck]::IsShadow($f.FullName)) { $shadow[$f.Name.Substring(0,16).ToLower()] = $true } } catch { }
}
Write-Progress -Activity "Checking dumped textures" -Completed
"$($shadow.Count) shadow textures found among $($files.Count) dumped textures."

# 2. Every file with one of those hashes: dumps, texture_new, packs.
$roots = @($dump) + @(if (Test-Path $new) { $new }) + @(if (Test-Path $packs) { $packs })
$moves = foreach ($r in $roots) {
    Get-ChildItem $r -Recurse -File -Include *.png,*.dds |
        Where-Object { $_.Name -match '^[0-9a-fA-F]{16}' -and $shadow.ContainsKey($_.Name.Substring(0,16).ToLower()) }
}
$bytes = ($moves | Measure-Object Length -Sum).Sum
"$(@($moves).Count) files to move ($([math]::Round($bytes / 1MB, 1)) MB)."

New-Item -ItemType Directory -Force $dest | Out-Null
$shadow.Keys | Sort-Object | Set-Content (Join-Path $dest "shadows.txt")
if ($ListOnly) { "List only: nothing moved. Hashes in $dest\shadows.txt"; exit 0 }

$moved = 0
foreach ($m in $moves) {
    $rel = $m.FullName.Substring($bin.Length).TrimStart('\', '/')
    $target = Join-Path $dest $rel
    New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
    if (Test-Path $target) { continue }   # never overwrite
    Move-Item -LiteralPath $m.FullName -Destination $target
    $moved++
}
"$moved files moved to $dest. Look them over, then delete that folder."
"In the game, click Reload under F9 > Options > Textures (or restart) so the pack's list is rebuilt."
