#Requires -Version 7
<#
.SYNOPSIS
    Make @2x versions of the Aero scrollbar arrow glyphs with waifu2x-ncnn-vulkan
    (ADR 0007: never shipped, never committed).

.DESCRIPTION
    aero.msstyles stores the scrollbar arrow glyphs at 96/120/144 dpi (IMAGE 718/719/720,
    13/16/20 px per state) and the thumb gripper at 96/120/144 dpi (IMAGE 724/725/726).
    This script extracts the selected set, unpremultiplies the theme's premultiplied alpha
    to straight alpha, upscales colour and alpha separately 2x and writes
    <profile>\glasswave\hd\aero\<id>@2x.png; the component's existing @2x artwork loader
    uses them.

    Only the selected glyphs are upscaled. The arrow-button plate (717), track (728) and
    thumb background (733) have no high-dpi theme images and stay as the theme's nine-grid
    art; the middle thumb background is deliberately left untouched.

.PARAMETER ProfileDir
    Portable foobar2000 profile; default the repository dev instance.

.PARAMETER Part
    Arrows (default): the up/down/left/right arrow glyphs. Gripper: the three horizontal
    grip lines on the thumb. Both: both sets.

.PARAMETER Waifu2x
    Path to waifu2x-ncnn-vulkan.exe. Default: the newest copy under .local\tools.

.PARAMETER Noise
    waifu2x denoise level, -1 (none, default: the sources are lossless) to 3.

.PARAMETER Model
    waifu2x model directory name; cunet is the project default.

.PARAMETER Tta
    Average the 8 flips / rotations of every image (slower, slightly cleaner edges).
#>
[CmdletBinding()]
param(
    [string]$ProfileDir = (Join-Path $PSScriptRoot '..\.local\fb2k-dev\profile'),
    [ValidateSet('Arrows', 'Gripper', 'Both')][string]$Part = 'Arrows',
    [string]$Waifu2x,
    [ValidateRange(-1, 3)][int]$Noise = -1,
    [ValidateSet('models-cunet', 'models-upconv_7_anime_style_art_rgb', 'models-upconv_7_photo')]
    [string]$Model = 'models-cunet',
    [bool]$Tta = $true,
    [int]$TileSize = 128
)
$ErrorActionPreference = 'Stop'

$art = Join-Path (Resolve-Path $ProfileDir) 'glasswave'
$theme = Join-Path $art 'aero.msstyles'
if (-not (Test-Path -LiteralPath $theme)) { throw "aero.msstyles not found in $art (run scripts/run-dev.ps1 first)" }

if (-not $Waifu2x) {
    $Waifu2x = Get-ChildItem (Join-Path $PSScriptRoot '..\.local\tools') -Recurse -Filter 'waifu2x-ncnn-vulkan.exe' -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $Waifu2x -or -not (Test-Path -LiteralPath $Waifu2x)) {
    throw 'waifu2x-ncnn-vulkan.exe not found; pass -Waifu2x or unpack a release under .local\tools'
}

Add-Type -AssemblyName System.Drawing
$null = [System.Drawing.Bitmap]
$refs = [AppDomain]::CurrentDomain.GetAssemblies() |
    Where-Object { -not $_.IsDynamic -and $_.Location -and $_.GetName().Name -match '^(System\.Drawing|System\.Private\.Windows|System\.Runtime|netstandard|System\.Collections|System\.Memory|System\.ComponentModel\.Primitives|System\.Runtime\.InteropServices)' } |
    ForEach-Object Location
Add-Type -ReferencedAssemblies $refs -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;
public static class AeroArrowArt {
    [DllImport("kernel32", CharSet = CharSet.Unicode)]
    static extern IntPtr LoadLibraryExW(string file, IntPtr reserved, uint flags);
    [DllImport("kernel32")] static extern bool FreeLibrary(IntPtr module);
    [DllImport("kernel32", CharSet = CharSet.Unicode)]
    static extern IntPtr FindResourceW(IntPtr module, IntPtr name, IntPtr type);
    [DllImport("kernel32")] static extern IntPtr LoadResource(IntPtr module, IntPtr res);
    [DllImport("kernel32")] static extern IntPtr LockResource(IntPtr data);
    [DllImport("kernel32")] static extern uint SizeofResource(IntPtr module, IntPtr res);

    public static IntPtr Open(string file) {
        IntPtr m = LoadLibraryExW(file, IntPtr.Zero, 0x22);  // datafile | image resource
        if (m == IntPtr.Zero) throw new IOException("cannot load " + file);
        return m;
    }
    public static void Close(IntPtr m) { FreeLibrary(m); }
    public static byte[] Extract(IntPtr m, int id) {
        IntPtr type = Marshal.StringToHGlobalUni("IMAGE");
        IntPtr res = FindResourceW(m, (IntPtr)id, type);
        Marshal.FreeHGlobal(type);
        if (res == IntPtr.Zero) return null;
        uint size = SizeofResource(m, res);
        byte[] bytes = new byte[size];
        Marshal.Copy(LockResource(LoadResource(m, res)), bytes, 0, (int)size);
        return bytes;
    }

    static byte[] Pixels(Bitmap b) {
        var d = b.LockBits(new Rectangle(0, 0, b.Width, b.Height), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        byte[] px = new byte[b.Width * b.Height * 4];
        for (int y = 0; y < b.Height; ++y)
            Marshal.Copy(d.Scan0 + y * d.Stride, px, y * b.Width * 4, b.Width * 4);
        b.UnlockBits(d);
        return px;
    }
    static Bitmap From(byte[] px, int w, int h) {
        var b = new Bitmap(w, h, PixelFormat.Format32bppArgb);
        var d = b.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
        for (int y = 0; y < h; ++y)
            Marshal.Copy(px, y * w * 4, d.Scan0 + y * d.Stride, w * 4);
        b.UnlockBits(d);
        return b;
    }

    // aero.msstyles glyphs store premultiplied colour; the @2x loader expects straight PNG.
    public static int[] Unpremultiply(byte[] png, string outPath) {
        using var src = new Bitmap(new MemoryStream(png));
        int w = src.Width, h = src.Height;
        byte[] px = Pixels(src);
        for (int i = 0; i < px.Length; i += 4) {
            int a = px[i + 3];
            if (a == 0) { px[i] = px[i + 1] = px[i + 2] = 0; }
            else if (a < 255) {
                for (int c = 0; c < 3; ++c)
                    px[i + c] = (byte)Math.Min(255, (px[i + c] * 255 + a / 2) / a);
            }
        }
        using (var b = From(px, w, h)) b.Save(outPath, ImageFormat.Png);
        return new[] { w, h };
    }

    const int Pad = 8;  // the upscaler dents shapes that touch the border
    public static void Split(byte[] png, string colorOut, string alphaOut) {
        using var src = new Bitmap(new MemoryStream(png));
        int w = src.Width, h = src.Height, pw = w + 2 * Pad, ph = h + 2 * Pad;
        byte[] px = Pixels(src), color = new byte[pw * ph * 4], alpha = new byte[pw * ph * 4];
        for (int i = 3; i < color.Length; i += 4) color[i] = alpha[i] = 255;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                int i = (y * w + x) * 4, o = ((y + Pad) * pw + x + Pad) * 4;
                int a = px[i + 3];
                for (int c = 0; c < 3; ++c) color[o + c] = (byte)((px[i + c] * a + 127) / 255);
                alpha[o] = alpha[o + 1] = alpha[o + 2] = (byte)a;
            }
        using (var b = From(color, pw, ph)) b.Save(colorOut, ImageFormat.Png);
        using (var b = From(alpha, pw, ph)) b.Save(alphaOut, ImageFormat.Png);
    }

    public static int[] Join(string colorIn, string alphaIn, string output) {
        using var cb = new Bitmap(colorIn);
        using var ab = new Bitmap(alphaIn);
        int pw = cb.Width, m = 2 * Pad, w = cb.Width - 2 * m, h = cb.Height - 2 * m;
        byte[] color = Pixels(cb), alpha = Pixels(ab), px = new byte[w * h * 4];
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                int i = ((y + m) * pw + x + m) * 4, o = (y * w + x) * 4;
                int a = (alpha[i] + alpha[i + 1] + alpha[i + 2] + 1) / 3;
                px[o + 3] = (byte)a;
                for (int c = 0; c < 3; ++c)
                    px[o + c] = a == 0 ? (byte)0 : (byte)Math.Min(255, (color[i + c] * 255 + a / 2) / a);
            }
        using (var b = From(px, w, h)) b.Save(output, ImageFormat.Png);
        return new[] { w, h };
    }
}
'@

$work = Join-Path ([IO.Path]::GetTempPath()) ("glasswave-arrows-" + [Guid]::NewGuid().ToString('N'))
$inDir = Join-Path $work 'in'; $outDir = Join-Path $work 'out'
New-Item -ItemType Directory -Force -Path $inDir, $outDir | Out-Null
$hdDir = Join-Path $art 'hd\aero'
New-Item -ItemType Directory -Force -Path $hdDir | Out-Null

# Arrow glyphs or the thumb gripper at 96/120/144 dpi; the plate/track/thumb stay original.
$ids = switch ($Part) {
    'Gripper' { 724, 725, 726 }
    'Both' { 718, 719, 720, 724, 725, 726 }
    default { 718, 719, 720 }
}

try {
    $sizes = @{}
    $module = [AeroArrowArt]::Open($theme)
    try {
        foreach ($id in $ids) {
            $png = [AeroArrowArt]::Extract($module, $id)
            if (-not $png) { Write-Warning "IMAGE $id not found, skipped"; continue }
            $straight = Join-Path $work "$id.straight.png"
            $sizes[$id] = [AeroArrowArt]::Unpremultiply($png, $straight)
            [AeroArrowArt]::Split([IO.File]::ReadAllBytes($straight), (Join-Path $inDir "$id.c.png"),
                                  (Join-Path $inDir "$id.a.png"))
        }
    } finally { [AeroArrowArt]::Close($module) }

    $modelDir = Join-Path (Split-Path $Waifu2x) $Model
    [string[]]$cmd = @('-i', $inDir, '-o', $outDir, '-s', '2', '-n', "$Noise", '-m', $modelDir, '-f', 'png',
                       '-t', "$TileSize")
    if ($Tta) { $cmd = @('-x') + $cmd }
    & $Waifu2x $cmd
    if ($LASTEXITCODE -ne 0) { throw "waifu2x failed with exit code $LASTEXITCODE" }

    foreach ($id in $sizes.Keys) {
        $c = Join-Path $outDir "$id.c.png"; $a = Join-Path $outDir "$id.a.png"
        if (-not (Test-Path -LiteralPath $c) -or -not (Test-Path -LiteralPath $a)) {
            Write-Warning "${id}: no output"; continue
        }
        $target = Join-Path $hdDir "$id@2x.png"
        $wh = [AeroArrowArt]::Join($c, $a, $target)
        $ew = 2 * $sizes[$id][0]; $eh = 2 * $sizes[$id][1]
        if ($wh[0] -ne $ew -or $wh[1] -ne $eh) {
            Remove-Item -LiteralPath $target
            Write-Warning "${id}: got $($wh[0])x$($wh[1]), expected ${ew}x${eh}; removed"
            continue
        }
        Write-Host "$id -> $($wh[0])x$($wh[1])"
    }
    Write-Host "Wrote @2x arrow glyphs to $hdDir; restart foobar2000 to load them."
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
