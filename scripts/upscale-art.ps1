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
    # waifu2x tile size; its automatic choice garbled the right part of wide bitmaps (the
    # 558 px transport capsule) on the owner's RTX 5070 Ti.
    [int]$TileSize = 128,
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
       (5244..5247) + (5678..5681) + (5350..5353) + @(5360) + (4723..4725) + @(331) + (4601..4603)
$names = @($ids | ForEach-Object { "#$_" }) + @('ALBUM_0.PNG', 'ALBUMDROPSHADOW_200X200.PNG')
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
               'OVERLAYS.VOLUME.BAR.ON.PNG', 'OVERLAYS.VOLUME.BAR.OFF.PNG', 'AUDIO.NOCOVER.PNG')
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

    // Splits a PNG into opaque premultiplied colour and opaque grey alpha, padded.
    public static int[] Split(byte[] png, string colorOut, string alphaOut) {
        using var src = new Bitmap(new MemoryStream(png));
        int w = src.Width, h = src.Height, pw = w + 2 * Pad, ph = h + 2 * Pad;
        byte[] px = Pixels(src), color = new byte[pw * ph * 4], alpha = new byte[pw * ph * 4];
        for (int i = 3; i < color.Length; i += 4) color[i] = alpha[i] = 255;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                int i = (y * w + x) * 4, o = ((y + Pad) * pw + x + Pad) * 4;  // BGRA
                int a = px[i + 3];
                for (int c = 0; c < 3; ++c) color[o + c] = (byte)((px[i + c] * a + 127) / 255);
                alpha[o] = alpha[o + 1] = alpha[o + 2] = (byte)a;
            }
        using (var b = FromPixels(color, pw, ph)) b.Save(colorOut, ImageFormat.Png);
        using (var b = FromPixels(alpha, pw, ph)) b.Save(alphaOut, ImageFormat.Png);
        return new[] { w, h };
    }

    // Recombines the upscaled halves into a straight-alpha PNG.
    public static int[] Join(string colorIn, string alphaIn, string output) {
        using var cb = new Bitmap(colorIn);
        using var ab = new Bitmap(alphaIn);
        if (cb.Width != ab.Width || cb.Height != ab.Height) throw new InvalidDataException("size mismatch");
        int pw = cb.Width, m = 2 * Pad, w = cb.Width - 2 * m, h = cb.Height - 2 * m;  // crop the 2x margin
        byte[] color = Pixels(cb), alpha = Pixels(ab), px = new byte[w * h * 4];
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                int i = ((y + m) * pw + x + m) * 4, o = (y * w + x) * 4;
                int a = (alpha[i] + alpha[i + 1] + alpha[i + 2] + 1) / 3;
                px[o + 3] = (byte)a;
                for (int c = 0; c < 3; ++c)
                    px[o + c] = a == 0 ? (byte)0 : (byte)Math.Min(255, (color[i + c] * 255 + a / 2) / a);
            }
        using (var b = FromPixels(px, w, h)) b.Save(output, ImageFormat.Png);
        return new[] { w, h };
    }
}
'@

$work = Join-Path ([IO.Path]::GetTempPath()) ("glasswave-upscale-" + [Guid]::NewGuid().ToString('N'))
$inDir = Join-Path $work 'in'; $outDir = Join-Path $work 'out'
New-Item -ItemType Directory -Force -Path $inDir, $outDir | Out-Null
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
            $sizes[$file] = [GlasswaveArt]::Split($png, (Join-Path $inDir "$file.c.png"), (Join-Path $inDir "$file.a.png"))
        }
    } finally { [GlasswaveArt]::Close($module) }

    $modelDir = Join-Path (Split-Path $Waifu2x) $Model
    Write-Host "Upscaling $($sizes.Count) bitmaps with $(Split-Path -Leaf $Waifu2x) ($Model, noise $Noise, TTA $Tta)"
    [string[]]$cmd = @('-i', $inDir, '-o', $outDir, '-s', '2', '-n', "$Noise", '-m', $modelDir, '-f', 'png', '-t', "$TileSize")
    if ($Tta) { $cmd = @('-x') + $cmd }
    & $Waifu2x $cmd
    if ($LASTEXITCODE -ne 0) { throw "waifu2x failed with exit code $LASTEXITCODE" }

    $written = 0
    foreach ($file in $sizes.Keys) {
        $c = Join-Path $outDir "$file.c.png"; $a = Join-Path $outDir "$file.a.png"
        if (-not (Test-Path -LiteralPath $c) -or -not (Test-Path -LiteralPath $a)) { Write-Warning "${file}: no output"; continue }
        $target = Join-Path $hdDir "$file@2x.png"
        $wh = [GlasswaveArt]::Join($c, $a, $target)
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
