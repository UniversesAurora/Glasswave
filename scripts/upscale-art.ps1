#Requires -Version 7
<#
.SYNOPSIS
    Make @2x versions of the WMP 12 and Media Center bitmaps Glasswave draws, from the
    user's own wmploc.dll / ehres.dll, with waifu2x-ncnn-vulkan (ADR 0007: never shipped,
    never committed).

.DESCRIPTION
    Extracts the wmploc.dll PNG resources the UI uses from <profile>\glasswave\wmploc.dll,
    upscales them 2x and writes <profile>\glasswave\hd\wmploc\<name>@2x.png, which the
    component loads at 192 DPI in place of the original (1 original pixel = 1 DIP, so 2x
    is exactly one image pixel per device pixel at 200 %).

    waifu2x works on RGB. Transparent pixels in these bitmaps carry arbitrary (mostly
    black) colour that would bleed into the edges, so colour and alpha are upscaled as two
    opaque images - premultiplied colour and alpha as grey - and divided back afterwards.

    Nine-grid art (LISTVIEWHEADER) is left alone: its borders are drawn 1:1 anyway.

.PARAMETER Source
    wmploc (default): the WMP 12 transport and list art. ehres: Media Center's windowed
    controls, seek bar and volume bar that the player window draws (hd\ehres).

.PARAMETER Waifu2x
    Path to waifu2x-ncnn-vulkan.exe. Default: the newest copy under .local\tools.

.PARAMETER Noise
    waifu2x denoise level, -1 (none, default: the sources are lossless) to 3.
#>
[CmdletBinding()]
param(
    [string]$ProfileDir = (Join-Path $PSScriptRoot '..\.local\fb2k-dev\profile'),
    [ValidateSet('wmploc', 'ehres')][string]$Source = 'wmploc',
    [string]$Waifu2x,
    [ValidateRange(-1, 3)][int]$Noise = -1,
    [ValidateSet('models-cunet', 'models-upconv_7_anime_style_art_rgb', 'models-upconv_7_photo')]
    [string]$Model = 'models-cunet',
    # TTA: average the 8 flips / rotations of every image (8x slower, slightly cleaner edges).
    [bool]$Tta = $true,
    # waifu2x tile size. On the owner's RTX 5070 Ti its automatic choice garbled the right part
    # of wide bitmaps (the 558 px transport capsule) and 128 garbled the left part of the flat
    # translucent OVERLAYS.OUTLINE into coloured noise; 64 is clean on both.
    [int]$TileSize = 64,
    # waifu2x device: 0 the first GPU, -1 the CPU (crashes in waifu2x-ncnn-vulkan 20220728).
    [int]$Gpu = 0,
    # Where the @2x files go; default <profile>\glasswave\hd\wmploc (for trying settings).
    [string]$OutputDir
)
$ErrorActionPreference = 'Stop'

$art = Join-Path (Resolve-Path $ProfileDir) 'glasswave'
$dll = Join-Path $art "$Source.dll"
if (-not (Test-Path -LiteralPath $dll)) { throw "$Source.dll not found in $art (run scripts/run-dev.ps1 first)" }

if (-not $Waifu2x) {
    $Waifu2x = Get-ChildItem (Join-Path $PSScriptRoot '..\.local\tools') -Recurse -Filter 'waifu2x-ncnn-vulkan.exe' -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $Waifu2x -or -not (Test-Path -LiteralPath $Waifu2x)) {
    throw 'waifu2x-ncnn-vulkan.exe not found; pass -Waifu2x or unpack a release under .local\tools'
}

# Resources drawn by wmp/wmp_transport.* and ui/list_view.cpp (type 257 in wmploc.dll).
$ids = @(4153, 4154, 4155, 4160) + (4162..4169) + (4174..4177) + @(4179, 4180, 2164) +
       (5244..5247) + (5678..5681) + (5350..5353) + @(5360) + (4723..4725) + @(331) + (4601..4603) +
       (4626..4628) + 4630  # the mini player's split volume button (its flyout slider is drawn in vectors)
$names = @($ids | ForEach-Object { "#$_" }) + @('ALBUM_0.PNG', 'ALBUMDROPSHADOW_200X200.PNG')
# Glyphs whose white halo (made for WMP's light capsule) would be taken out; none for now, the
# owner keeps WMP's halo.
$dehalo = @()
$resourceType = 257
if ($Source -eq 'ehres') {
    # Resources drawn by ui/player_window.cpp (type 10, RCDATA). Gradients and backgrounds are
    # left alone: they are soft already.
    $resourceType = 10
    $controls = 'STOP', 'PREVIOUS', 'NEXT', 'PLAY', 'PAUSE', 'MUTE', 'MUTEON', 'MINUS', 'PLUS', 'GUIDE',
                'TELETEXT', 'EXIT', 'MINIMIZE', 'RESTORE', 'GOBACK', 'DOWN'
    $names = @(foreach ($control in $controls) {
            foreach ($state in 'DISABLED', 'FOCUS', 'NOFOCUS', 'PRESSED') { "TWOFOOT.CONTROL.$control.$state.PNG" }
        }) + @('TWOFOOT.CONTROL.DIVIDER.PNG', 'SEEKBAR.PAST.PNG', 'SEEKBAR.FUTURE.PNG',
               'SEEKBAR.PROGRESS.BACKGROUND.PNG', 'SEEKBAR.POSITION.ACTIVE.PNG', 'SEEKBAR.POSITION.DORMANT.PNG',
               'OVERLAYS.VOLUME.BAR.ON.PNG', 'OVERLAYS.VOLUME.BAR.OFF.PNG', 'OVERLAYS.OUTLINE.PNG',
               'OVERLAYS.VOLUME.MUTE.PNG', 'AUDIO.NOCOVER.PNG')
}

Add-Type -AssemblyName System.Drawing
$null = [System.Drawing.Bitmap]  # load GDI+ and its helper assemblies
$refs = [AppDomain]::CurrentDomain.GetAssemblies() |
    Where-Object { -not $_.IsDynamic -and $_.Location -and $_.GetName().Name -match '^(System\.Drawing|System\.Private\.Windows|System\.Runtime|netstandard|System\.Collections|System\.Memory|System\.ComponentModel\.Primitives|System\.Runtime\.InteropServices)' } |
    ForEach-Object Location
Add-Type -ReferencedAssemblies $refs -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;

public static class GlasswaveArt {
    [DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr LoadLibraryExW(string file, IntPtr reserved, uint flags);
    [DllImport("kernel32")] static extern bool FreeLibrary(IntPtr module);
    [DllImport("kernel32", CharSet = CharSet.Unicode)]
    static extern IntPtr FindResourceW(IntPtr module, IntPtr name, IntPtr type);
    [DllImport("kernel32", CharSet = CharSet.Unicode, EntryPoint = "FindResourceW")]
    static extern IntPtr FindResourceNamed(IntPtr module, string name, IntPtr type);
    [DllImport("kernel32")] static extern IntPtr LoadResource(IntPtr module, IntPtr res);
    [DllImport("kernel32")] static extern IntPtr LockResource(IntPtr data);
    [DllImport("kernel32")] static extern uint SizeofResource(IntPtr module, IntPtr res);

    // LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE: no code runs.
    public static IntPtr Open(string file) {
        IntPtr m = LoadLibraryExW(file, IntPtr.Zero, 0x22);
        if (m == IntPtr.Zero) throw new IOException("cannot load " + file);
        return m;
    }
    public static void Close(IntPtr m) { FreeLibrary(m); }

    public static byte[] Extract(IntPtr m, string name, int type) {
        IntPtr res = name.StartsWith("#")
            ? FindResourceW(m, (IntPtr)int.Parse(name.Substring(1)), (IntPtr)type)
            : FindResourceNamed(m, name, (IntPtr)type);
        if (res == IntPtr.Zero) return null;
        uint size = SizeofResource(m, res);
        byte[] bytes = new byte[size];
        Marshal.Copy(LockResource(LoadResource(m, res)), bytes, 0, (int)size);
        return bytes;
    }

    static byte[] Pixels(Bitmap b) {
        var data = b.LockBits(new Rectangle(0, 0, b.Width, b.Height), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        byte[] px = new byte[b.Width * b.Height * 4];
        for (int y = 0; y < b.Height; ++y)
            Marshal.Copy(data.Scan0 + y * data.Stride, px, y * b.Width * 4, b.Width * 4);
        b.UnlockBits(data);
        return px;
    }
    static Bitmap FromPixels(byte[] px, int w, int h) {
        var b = new Bitmap(w, h, PixelFormat.Format32bppArgb);
        var data = b.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
        for (int y = 0; y < h; ++y)
            Marshal.Copy(px, y * w * 4, data.Scan0 + y * data.Stride, w * 4);
        b.UnlockBits(data);
        return b;
    }

    // The upscaler is poor at image edges (shapes touching the border come out dented and
    // soft), so every image gets a transparent margin of Pad pixels, cropped off afterwards.
    public const int Pad = 8;

    // Splits a PNG into opaque colour and opaque grey alpha, padded. The colour is the straight
    // (not premultiplied) one: premultiplied, a faint translucent image keeps only a few levels
    // per channel and dividing by alpha afterwards turns the upscaler's error into coloured
    // noise. Transparent pixels take the colour of their nearest neighbours instead, so no dark
    // fringe is pulled into the edges.
    // Takes out a light halo drawn round a glyph for a light background (WMP's speaker glyphs):
    // a pixel within two pixels of the transparent outside is taken as a blend of white over
    // the nearest opaque colour inside it; the white share leaves its alpha, the colour stays.
    // Opaque white inside a glyph (the bar of the no-entry sign) is untouched.
    static void Dehalo(byte[] px, int w, int h) {
        var src = (byte[])px.Clone();
        Func<int, int, int> a = (x, y) => x < 0 || y < 0 || x >= w || y >= h ? 0 : src[(y * w + x) * 4 + 3];
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                int i = (y * w + x) * 4;
                if (src[i + 3] == 0) continue;
                bool edge = false;
                for (int dy = -2; dy <= 2 && !edge; ++dy)
                    for (int dx = -2; dx <= 2; ++dx)
                        if (a(x + dx, y + dy) == 0) { edge = true; break; }
                if (!edge) continue;
                int best = -1, bestD = int.MaxValue;
                for (int dy = -3; dy <= 3; ++dy)
                    for (int dx = -3; dx <= 3; ++dx) {
                        int qx = x + dx, qy = y + dy;
                        if (a(qx, qy) < 250) continue;
                        int q = (qy * w + qx) * 4;
                        if (src[q] > 230 && src[q + 1] > 230 && src[q + 2] > 230) continue; // white is no core
                        int d = dx * dx + dy * dy;
                        if (d < bestD) { bestD = d; best = q; }
                    }
                double t = 1.0;  // no colour near: all halo
                if (best >= 0) {
                    double sum = 0; int n = 0;
                    for (int c = 0; c < 3; ++c) {
                        double room = 255 - src[best + c];
                        if (room < 40) continue;
                        sum += Math.Clamp((src[i + c] - src[best + c]) / room, 0, 1); ++n;
                    }
                    t = n > 0 ? sum / n : 0;
                    for (int c = 0; c < 3; ++c) px[i + c] = src[best + c];
                }
                px[i + 3] = (byte)Math.Round(src[i + 3] * (1 - t));
            }
    }

    public static int[] Split(byte[] png, string colorOut, string alphaOut, string coverageOut, bool dehalo) {
        using var src = new Bitmap(new MemoryStream(png));
        int w = src.Width, h = src.Height, pw = w + 2 * Pad, ph = h + 2 * Pad;
        byte[] px = Pixels(src), color = new byte[pw * ph * 4], alpha = new byte[pw * ph * 4];
        if (dehalo) Dehalo(px, w, h);
        bool[] known = new bool[pw * ph];
        for (int i = 3; i < color.Length; i += 4) color[i] = alpha[i] = 255;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                int i = (y * w + x) * 4, o = ((y + Pad) * pw + x + Pad) * 4;  // BGRA
                int a = px[i + 3];
                if (a > 0) {
                    for (int c = 0; c < 3; ++c) color[o + c] = px[i + c];
                    known[o / 4] = true;
                }
                alpha[o] = alpha[o + 1] = alpha[o + 2] = (byte)a;
            }
        // Grow the known colours outwards one pixel per pass until every pixel has one.
        for (bool grew = true; grew;) {
            grew = false;
            bool[] next = (bool[])known.Clone();
            for (int y = 0; y < ph; ++y)
                for (int x = 0; x < pw; ++x) {
                    int p = y * pw + x;
                    if (known[p]) continue;
                    int n = 0, b0 = 0, g0 = 0, r0 = 0;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            int qx = x + dx, qy = y + dy;
                            if (qx < 0 || qy < 0 || qx >= pw || qy >= ph || !known[qy * pw + qx]) continue;
                            int q = (qy * pw + qx) * 4;
                            b0 += color[q]; g0 += color[q + 1]; r0 += color[q + 2]; ++n;
                        }
                    if (n == 0) continue;
                    color[p * 4] = (byte)(b0 / n); color[p * 4 + 1] = (byte)(g0 / n); color[p * 4 + 2] = (byte)(r0 / n);
                    next[p] = true;
                    grew = true;
                }
            known = next;
        }
        using (var b = FromPixels(color, pw, ph)) b.Save(colorOut, ImageFormat.Png);
        using (var b = FromPixels(alpha, pw, ph)) b.Save(alphaOut, ImageFormat.Png);
        // The original coverage, unpadded, for Join to hold the upscaled alpha to.
        byte[] coverage = new byte[w * h * 4];
        for (int i = 0; i < w * h; ++i) { coverage[i * 4] = coverage[i * 4 + 1] = coverage[i * 4 + 2] = px[i * 4 + 3]; coverage[i * 4 + 3] = 255; }
        using (var b = FromPixels(coverage, w, h)) b.Save(coverageOut, ImageFormat.Png);
        return new[] { w, h };
    }

    // Recombines the upscaled halves into a straight-alpha PNG. The upscaler sharpens alpha
    // like any image and turns faint edges solid (WMP's speaker halo, 57 % opaque, came out at
    // 94 %); each 2x2 block is therefore scaled back to its original pixel's coverage, keeping
    // the upscaled shape inside it.
    public static int[] Join(string colorIn, string alphaIn, string coverageIn, string output) {
        using var cb = new Bitmap(colorIn);
        using var ab = new Bitmap(alphaIn);
        using var ob = new Bitmap(coverageIn);
        if (cb.Width != ab.Width || cb.Height != ab.Height) throw new InvalidDataException("size mismatch");
        int pw = cb.Width, m = 2 * Pad, w = cb.Width - 2 * m, h = cb.Height - 2 * m;  // crop the 2x margin
        if (ob.Width * 2 != w || ob.Height * 2 != h) throw new InvalidDataException("coverage size mismatch");
        byte[] color = Pixels(cb), alpha = Pixels(ab), cover = Pixels(ob), px = new byte[w * h * 4];
        int[] a = new int[w * h];
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                int i = ((y + m) * pw + x + m) * 4;
                a[y * w + x] = (alpha[i] + alpha[i + 1] + alpha[i + 2] + 1) / 3;
            }
        // Where the original is see-through (alpha under 200: halos, glows, soft shadows) the
        // upscaled alpha is the original's, smoothly interpolated, so it keeps its even
        // translucency; the edges of solid shapes keep the upscaler's sharp alpha.
        int ow = w / 2, oh = h / 2;
        Func<int, int, double> orig = (x, y) => cover[(Math.Clamp(y, 0, oh - 1) * ow + Math.Clamp(x, 0, ow - 1)) * 4];
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                if (orig(x / 2, y / 2) >= 200) continue;
                double sx = (x + 0.5) / 2 - 0.5, sy = (y + 0.5) / 2 - 0.5;
                int x0 = (int)Math.Floor(sx), y0 = (int)Math.Floor(sy);
                double fx = sx - x0, fy = sy - y0;
                double v = orig(x0, y0) * (1 - fx) * (1 - fy) + orig(x0 + 1, y0) * fx * (1 - fy) +
                           orig(x0, y0 + 1) * (1 - fx) * fy + orig(x0 + 1, y0 + 1) * fx * fy;
                a[y * w + x] = (int)Math.Round(v);
            }
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                int i = ((y + m) * pw + x + m) * 4, o = (y * w + x) * 4;
                int av = a[y * w + x];
                px[o + 3] = (byte)av;
                for (int c = 0; c < 3; ++c) px[o + c] = av == 0 ? (byte)0 : color[i + c];
            }
        using (var b = FromPixels(px, w, h)) b.Save(output, ImageFormat.Png);
        return new[] { w, h };
    }
}
'@

$work = Join-Path ([IO.Path]::GetTempPath()) ("glasswave-upscale-" + [Guid]::NewGuid().ToString('N'))
$inDir = Join-Path $work 'in'; $outDir = Join-Path $work 'out'; $covDir = Join-Path $work 'coverage'
New-Item -ItemType Directory -Force -Path $inDir, $outDir, $covDir | Out-Null
$hdDir = if ($OutputDir) { $OutputDir } else { Join-Path $art "hd\$Source" }
New-Item -ItemType Directory -Force -Path $hdDir | Out-Null

try {
    $sizes = @{}
    $module = [GlasswaveArt]::Open($dll)
    try {
        foreach ($name in $names) {
            $png = [GlasswaveArt]::Extract($module, $name, $resourceType)
            if (-not $png) { continue } # not every control has every state
            $file = $name.TrimStart('#')
            $sizes[$file] = [GlasswaveArt]::Split($png, (Join-Path $inDir "$file.c.png"), (Join-Path $inDir "$file.a.png"),
                                                  (Join-Path $covDir "$file.o.png"), $dehalo -contains $name)
        }
    } finally { [GlasswaveArt]::Close($module) }

    $modelDir = Join-Path (Split-Path $Waifu2x) $Model
    Write-Host "Upscaling $($sizes.Count) bitmaps with $(Split-Path -Leaf $Waifu2x) ($Model, noise $Noise, TTA $Tta)"
    [string[]]$cmd = @('-i', $inDir, '-o', $outDir, '-s', '2', '-n', "$Noise", '-m', $modelDir, '-f', 'png', '-t', "$TileSize", '-g', "$Gpu")
    if ($Tta) { $cmd = @('-x') + $cmd }
    & $Waifu2x $cmd
    if ($LASTEXITCODE -ne 0) { throw "waifu2x failed with exit code $LASTEXITCODE" }

    $written = 0
    foreach ($file in $sizes.Keys) {
        $c = Join-Path $outDir "$file.c.png"; $a = Join-Path $outDir "$file.a.png"
        if (-not (Test-Path -LiteralPath $c) -or -not (Test-Path -LiteralPath $a)) { Write-Warning "${file}: no output"; continue }
        $target = Join-Path $hdDir "$file@2x.png"
        $wh = [GlasswaveArt]::Join($c, $a, (Join-Path $covDir "$file.o.png"), $target)
        $src = $sizes[$file]
        if ($wh[0] -ne 2 * $src[0] -or $wh[1] -ne 2 * $src[1]) {
            Remove-Item -LiteralPath $target
            Write-Warning "${file}: got $($wh[0])x$($wh[1]), expected exactly 2x $($src[0])x$($src[1]); removed"
            continue
        }
        ++$written
    }
    Write-Host "Wrote $written @2x bitmaps to $hdDir"
    Write-Host 'Restart foobar2000 to load them; delete that folder to go back to the originals.'
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
