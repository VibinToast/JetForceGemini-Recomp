# Moves the dumped textures that need upscaling (again) into texture_reupscale, using the dump's draw_trace.txt.
#
#   .\move reupscale textures.ps1                        (pack: bin\texture_packs\4K-Toasty-HD)
#   .\move reupscale textures.ps1 -ListOnly              only count them and write the list, move nothing
#   .\move reupscale textures.ps1 -Pack 16x_LSDIRDAT     another folder in bin\texture_packs
#   .\move reupscale textures.ps1 -Bin D:\JFG\bin        a bin folder somewhere else
#
# Lives in tools\textures (bin is build output and not part of the repository); works on ..\..\bin by default.
#
# Why: packs made before the game wrote draw_trace.txt were upscaled as if every texture were clamped, each on its own.
# With the trace, Toasty_Upscaler treats some textures differently, and only those (plus textures the pack does not
# have yet) need upscaling again. Every other texture would come out the same as the one already in the pack, so it
# stays where it is. A texture is picked when (reasons as written in reupscale.txt):
#   new      the pack has no file with its hash yet;
#   wrap     the game repeats or mirrors it (most of its draws use W or M on an axis): it gets a border of its
#            repeated / mirrored self before upscaling, so the copies meet without seams;
#   tile3d   drawn in 3D, clamped on an axis, and its opposite edges match there (ground, water, walls the game
#            repeats one copy per polygon): it gets a wrap border on that axis too (Toasty's "tile_pad_3d", on by
#            default; -NoTile3D leaves these out if it is off);
#   picture  a 2D piece of a bigger picture (a rectangle showing its whole texture, right next to another one at the
#            same scale in the same frame: splash screens, menus): pieces are joined and upscaled as one image;
#   hidden   only ever drawn as 2D rectangles that show part of it: the rest is stale texture memory that Toasty
#            replaces with the shown edge before upscaling.
# The rules follow Toasty_Upscaler's own (toasty/core.py: cmd_pad, cmd_stitch) but are simpler, and when unsure they
# pick a texture rather than leave it: a few textures may be upscaled again for nothing, none should be missed.
#
# What it does: reads every draw_trace.txt in texture_dump (also the per-level sub-folders of "One folder per level"),
# moves one file per picked hash from texture_dump to bin\texture_reupscale (never overwriting), and puts a copy of the
# traces (joined into one draw_trace.txt) and of texture_info.txt next to them, so Toasty upscales that folder as a
# traced dump. reupscale.txt lists each picked hash and why. Nothing is deleted; the pack is not touched.
#
# Afterwards: upscale bin\texture_reupscale with Toasty, put the results in the pack folder under the same names
# (replacing the old files; same hash, so the pack's rt64.json entries stay right, and the game rebuilds the list of a
# folder pack itself), then remake the .rtz if you use one.

param(
    [string]$Pack = "4K-Toasty-HD",
    [string]$Bin = "",
    [switch]$ListOnly,
    [switch]$NoTile3D
)

# The game's bin folder: by default the one next to this repository's tools folder (tools\textures\..\..\bin).
if (-not $Bin) { $Bin = Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) "bin" }
$Bin = (Resolve-Path -LiteralPath $Bin -ErrorAction SilentlyContinue).Path
if (-not $Bin) { Write-Error "bin folder not found. Build the game first, or pass -Bin <path to the bin folder>."; exit 1 }
$bin  = $Bin
$dump = Join-Path $bin "texture_dump"
$dest = Join-Path $bin "texture_reupscale"
$packDir = if ([System.IO.Path]::IsPathRooted($Pack)) { $Pack } else { Join-Path (Join-Path $bin "texture_packs") $Pack }

if (-not (Test-Path $packDir)) { Write-Error "Pack folder not found: $packDir"; exit 1 }
if (-not (Test-Path $dump))    { Write-Error "Dump folder not found: $dump"; exit 1 }

# Trace reading and the PNG check in C# (fast, and needs nothing but .NET). Dumps are 8-bit RGBA, not interlaced.
Add-Type -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Text.RegularExpressions;

public class JfgTrace {
    // Per hash: draws by wrap mode ("WC" = ws W, wt C), drawn as rectangles / triangles, most of it a rectangle shows,
    // and whether it is a piece of a bigger picture.
    public Dictionary<string, Dictionary<string, int>> Modes = new Dictionary<string, Dictionary<string, int>>();
    public HashSet<string> Rect = new HashSet<string>();
    public HashSet<string> Tris = new HashSet<string>();
    public Dictionary<string, int[]> Shown = new Dictionary<string, int[]>();
    public Dictionary<string, int[]> Size = new Dictionary<string, int[]>();
    public HashSet<string> Picture = new HashSet<string>();
    public int Lines;

    class Draw { public int Fb, X0, X1, Y0, Y1, Dsdx, Dtdy, W, H; public string Tex; }

    static readonly Regex Line = new Regex(
        @"^frame (\d+) fb (\d+) call \d+ (?:rect x (-?\d+)\.\.(-?\d+) y (-?\d+)\.\.(-?\d+) dsdx (-?\d+) dtdy (-?\d+)|(?:tris )?tris \d+) " +
        @"tex ([0-9a-f]{16}) size (\d+)x(\d+) ws ([WMC]) wt ([WMC])", RegexOptions.Compiled);

    // Texels a rectangle side samples: its length in pixels (quarter pixels / 4) times texels per pixel (step / 1024).
    static double Sampled(int quarterPixels, int step) { return quarterPixels / 4.0 * Math.Abs(step) / 1024.0; }

    // A rectangle that shows its whole texture (at 1:1, or scaled the same on both axes), or at 1:1 at least 3/4 of each
    // side of it from its top-left: a candidate piece of a bigger picture (Toasty: is_slice, is_part_slice).
    static bool IsPiece(Draw d) {
        if (d.X0 % 4 != 0 || d.X1 % 4 != 0 || d.Y0 % 4 != 0 || d.Y1 % 4 != 0) return false;
        int w = (d.X1 - d.X0) / 4, h = (d.Y1 - d.Y0) / 4;
        if (Math.Abs(d.Dsdx) == 1024 && Math.Abs(d.Dtdy) == 1024)
            return w <= d.W && h <= d.H && w * 4 >= d.W * 3 && h * 4 >= d.H * 3;
        if (Math.Abs(d.Dsdx) != Math.Abs(d.Dtdy) || d.Dsdx == 0) return false;
        double sx = Sampled(d.X1 - d.X0, d.Dsdx), sy = Sampled(d.Y1 - d.Y0, d.Dtdy);
        return d.W - 2 <= sx && sx <= d.W + 0.5 && d.H - 2 <= sy && sy <= d.H + 0.5;
    }

    static bool Touch(Draw a, Draw b) {
        int ox = Math.Min(a.X1, b.X1) - Math.Max(a.X0, b.X0), oy = Math.Min(a.Y1, b.Y1) - Math.Max(a.Y0, b.Y0);
        bool sideBySide = (a.X1 == b.X0 || b.X1 == a.X0) && oy > 0;
        bool stacked = (a.Y1 == b.Y0 || b.Y1 == a.Y0) && ox > 0;
        return sideBySide || stacked;
    }

    // Pieces of one frame that sit right against another piece at the same scale in the same framebuffer.
    void Pictures(List<Draw> pieces) {
        for (int i = 0; i < pieces.Count; i++) {
            for (int j = i + 1; j < pieces.Count; j++) {
                Draw a = pieces[i], b = pieces[j];
                if (a.Fb != b.Fb || Math.Abs(a.Dsdx) != Math.Abs(b.Dsdx) || Math.Abs(a.Dtdy) != Math.Abs(b.Dtdy)) continue;
                if (Touch(a, b)) { Picture.Add(a.Tex); Picture.Add(b.Tex); }
            }
        }
        pieces.Clear();
    }

    // One draw_trace.txt. Each play session in it counts its frames from 0 again; a frame is a new one whenever the
    // number changes, so sessions never mix.
    public void Read(string path) {
        List<Draw> frame = new List<Draw>();
        string current = null;
        foreach (string line in File.ReadLines(path)) {
            Match m = Line.Match(line);
            if (!m.Success) continue;
            Lines++;
            string frameNumber = m.Groups[1].Value;
            if (frameNumber != current) { Pictures(frame); current = frameNumber; }
            string tex = m.Groups[9].Value;
            int w = int.Parse(m.Groups[10].Value), h = int.Parse(m.Groups[11].Value);
            Size[tex] = new int[] { w, h };
            string mode = m.Groups[12].Value + m.Groups[13].Value;
            Dictionary<string, int> modes;
            if (!Modes.TryGetValue(tex, out modes)) { modes = new Dictionary<string, int>(); Modes[tex] = modes; }
            int count;
            modes.TryGetValue(mode, out count);
            modes[mode] = count + 1;
            if (!m.Groups[3].Success) { Tris.Add(tex); continue; }
            Rect.Add(tex);
            Draw d = new Draw();
            d.Fb = int.Parse(m.Groups[2].Value);
            d.X0 = int.Parse(m.Groups[3].Value); d.X1 = int.Parse(m.Groups[4].Value);
            d.Y0 = int.Parse(m.Groups[5].Value); d.Y1 = int.Parse(m.Groups[6].Value);
            d.Dsdx = int.Parse(m.Groups[7].Value); d.Dtdy = int.Parse(m.Groups[8].Value);
            d.W = w; d.H = h; d.Tex = tex;
            int sw = Math.Min(w, (int)Math.Ceiling(Sampled(d.X1 - d.X0, d.Dsdx) - 0.01));
            int sh = Math.Min(h, (int)Math.Ceiling(Sampled(d.Y1 - d.Y0, d.Dtdy) - 0.01));
            int[] old;
            if (Shown.TryGetValue(tex, out old)) { old[0] = Math.Max(old[0], sw); old[1] = Math.Max(old[1], sh); }
            else Shown[tex] = new int[] { sw, sh };
            if (IsPiece(d)) frame.Add(d);
        }
        Pictures(frame);
    }

    // The wrap mode most of the texture's draws use, as Toasty takes it ("CC" when it is not in the trace).
    public string Mode(string tex) {
        Dictionary<string, int> modes;
        if (!Modes.TryGetValue(tex, out modes)) return "CC";
        string best = "CC"; int most = -1;
        foreach (KeyValuePair<string, int> kv in modes) if (kv.Value > most) { best = kv.Key; most = kv.Value; }
        return best;
    }

    static uint BE(byte[] b, int o) { return (uint)(b[o] << 24 | b[o + 1] << 16 | b[o + 2] << 8 | b[o + 3]); }

    // RGBA8 pixels of a dumped PNG (null if it is not 8-bit RGBA, not interlaced); w and h are set.
    public static byte[] LoadRgba(string path, out int w, out int h) {
        byte[] f = File.ReadAllBytes(path);
        w = h = 0;
        if (f.Length < 33 || f[1] != (byte)'P' || f[2] != (byte)'N' || f[3] != (byte)'G') return null;
        w = (int)BE(f, 16); h = (int)BE(f, 20);
        if (f[24] != 8 || f[25] != 6 || f[28] != 0) return null;
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
        if (z.Length < 3) return null;
        int stride = w * 4;
        byte[] raw = new byte[(stride + 1) * h];
        using (DeflateStream ds = new DeflateStream(new MemoryStream(z, 2, z.Length - 2), CompressionMode.Decompress)) {
            int got = 0, n;
            while (got < raw.Length && (n = ds.Read(raw, got, raw.Length - got)) > 0) got += n;
            if (got < raw.Length) return null;
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
        return img;
    }

    // True if the texture continues from one edge to the opposite one (horizontal: left-right, else top-bottom): the
    // seam between the last and first column (row) is no bigger than a step between neighbours (Toasty: tiles_across,
    // seam <= 2 or seam <= 2 x the average step).
    public static bool TilesAcross(byte[] img, int w, int h, bool horizontal) {
        int n = horizontal ? w : h, m = horizontal ? h : w;
        if (n < 2) return false;
        double seam = 0, step = 0;
        for (int i = 0; i < m; i++) {
            for (int k = 0; k < n; k++) {
                int x = horizontal ? k : i, y = horizontal ? i : k;
                int x2 = horizontal ? (k + 1) % n : i, y2 = horizontal ? i : (k + 1) % n;
                for (int ch = 0; ch < 4; ch++) {
                    int diff = Math.Abs(img[(y2 * w + x2) * 4 + ch] - img[(y * w + x) * 4 + ch]);
                    if (k == n - 1) seam += diff; else step += diff;
                }
            }
        }
        seam /= (double)m * 4;
        step /= (double)m * (n - 1) * 4;
        return seam <= 2.0 || seam <= 2.0 * step;
    }
}
"@

# 1. The traces.
$traces = @(Get-ChildItem $dump -Recurse -File -Filter draw_trace.txt)
if ($traces.Count -eq 0) { Write-Error "No draw_trace.txt in $dump. Dump with the current build (it writes one), then run this again."; exit 1 }
$trace = New-Object JfgTrace
foreach ($t in $traces) { $trace.Read($t.FullName) }
"Trace: $($trace.Lines) draws of $($trace.Modes.Count) textures in $($traces.Count) draw_trace.txt file(s)."

# 2. Hashes that already have an upscaled file in the pack.
$done = @{}
Get-ChildItem $packDir -Recurse -File -Include *.png,*.dds | Where-Object { $_.Name -match '^[0-9a-fA-F]{16}' } |
    ForEach-Object { $done[$_.Name.Substring(0,16).ToLower()] = $true }
"Pack: $packDir ($($done.Count) textures)"

# 3. The dumped textures, one per hash (not _dynamic: textures the game redraws on most frames, never matched again).
$dumped = [ordered]@{}
Get-ChildItem $dump -Recurse -File -Filter *.png |
    Where-Object { $_.Name -match '^[0-9a-fA-F]{16}' -and $_.FullName -notmatch '[\\/]_dynamic[\\/]' } |
    ForEach-Object { $h = $_.Name.Substring(0,16).ToLower(); if (-not $dumped.Contains($h)) { $dumped[$h] = $_ } }

# 4. Why each one needs upscaling, if it does.
$picked = [ordered]@{}
$counts = [ordered]@{ new = 0; wrap = 0; tile3d = 0; picture = 0; hidden = 0 }
$i = 0
foreach ($h in $dumped.Keys) {
    $i++
    if ($i % 500 -eq 0) { Write-Progress -Activity "Checking dumped textures" -Status "$i / $($dumped.Count)" -PercentComplete (100 * $i / $dumped.Count) }
    $why = @()
    if (-not $done.ContainsKey($h)) { $why += "new" }
    $mode = $trace.Mode($h)
    $wrapS = $mode[0] -ne 'C'
    $wrapT = $mode[1] -ne 'C'
    if ($wrapS -or $wrapT) { $why += "wrap" }
    if ($trace.Picture.Contains($h)) { $why += "picture" }
    if ($trace.Rect.Contains($h) -and -not $trace.Tris.Contains($h)) {
        $shown = $trace.Shown[$h]; $size = $trace.Size[$h]
        if ((-not $wrapS -and $shown[0] -lt $size[0]) -or (-not $wrapT -and $shown[1] -lt $size[1])) { $why += "hidden" }
    }
    if (-not $NoTile3D -and $trace.Tris.Contains($h) -and -not ($wrapS -and $wrapT)) {
        $w = 0; $hh = 0
        $img = $null
        try { $img = [JfgTrace]::LoadRgba($dumped[$h].FullName, [ref]$w, [ref]$hh) } catch { }
        if ($img -and ((-not $wrapS -and [JfgTrace]::TilesAcross($img, $w, $hh, $true)) -or (-not $wrapT -and [JfgTrace]::TilesAcross($img, $w, $hh, $false)))) { $why += "tile3d" }
    }
    if ($why.Count -gt 0) {
        $picked[$h] = $why
        foreach ($r in $why) { $counts[$r]++ }
    }
}
Write-Progress -Activity "Checking dumped textures" -Completed

"$($picked.Count) of $($dumped.Count) dumped textures need upscaling:"
foreach ($r in $counts.Keys) { "  {0,-8} {1}" -f $r, $counts[$r] }
"($($dumped.Count - $picked.Count) are in the pack already and would come out the same.)"

New-Item -ItemType Directory -Force $dest | Out-Null
$picked.Keys | ForEach-Object { "$_ $($picked[$_] -join ',')" } | Set-Content (Join-Path $dest "reupscale.txt")
if ($ListOnly) { "List only: nothing moved. Hashes and reasons in $dest\reupscale.txt"; exit 0 }

# 5. Move them, with the traces and texture_info.txt copied next to them for Toasty.
$moved = 0
foreach ($h in $picked.Keys) {
    $f = $dumped[$h]
    $target = Join-Path $dest $f.Name
    if (Test-Path $target) { continue }   # never overwrite
    Move-Item -LiteralPath $f.FullName -Destination $target
    $moved++
}
$traces | ForEach-Object { Get-Content -LiteralPath $_.FullName } | Set-Content (Join-Path $dest "draw_trace.txt")
$infos = @(Get-ChildItem $dump -Recurse -File -Filter texture_info.txt)
if ($infos.Count -gt 0) { $infos | ForEach-Object { Get-Content -LiteralPath $_.FullName } | Set-Content (Join-Path $dest "texture_info.txt") }
"$moved textures moved to $dest (with draw_trace.txt and texture_info.txt). Upscale that folder with Toasty, then put"
"the results in $packDir under the same names."
