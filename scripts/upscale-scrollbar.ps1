#Requires -Version 7
<#
.SYNOPSIS
    Make @2x versions of the Aero scrollbar track/arrow-plate/thumb from the user's own
    aero.msstyles with waifu2x-ncnn-vulkan (ADR 0007: never shipped, never committed).

.DESCRIPTION
    The Win7 Aero theme only stores 96-DPI images for the scrollbar track (IMAGE 728),
    arrow-button plate (717) and thumb (733); at 125/150 % Windows stretches them. This
    script extracts those opaque 24-bit parts, upscales them 2x and writes
    <profile>\glasswave\hd\aero\<id>@2x.png, which the component loads in place of the
    original (the existing @2x artwork mechanism).

    The arrow glyphs (718/719/720) and grippers (724/725/726) already have native
    120/144-DPI resources and are not upscaled.

.PARAMETER ProfileDir
    Portable foobar2000 profile; default the repository dev instance.

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

Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.Runtime.InteropServices;
public static class AeroScrollArt {
    [DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)]
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
}
"@

$work = Join-Path ([IO.Path]::GetTempPath()) ("glasswave-scrollbar-" + [Guid]::NewGuid().ToString('N'))
$inDir = Join-Path $work 'in'; $outDir = Join-Path $work 'out'
New-Item -ItemType Directory -Force -Path $inDir, $outDir | Out-Null
$hdDir = Join-Path $art 'hd\aero'
New-Item -ItemType Directory -Force -Path $hdDir | Out-Null

# Track, arrow plate, thumb: the only scrollbar parts without native high-DPI images.
$ids = 717, 728, 733

try {
    Add-Type -AssemblyName System.Drawing
    $sizes = @{}
    $module = [AeroScrollArt]::Open($theme)
    try {
        foreach ($id in $ids) {
            $png = [AeroScrollArt]::Extract($module, $id)
            if (-not $png) { Write-Warning "IMAGE $id not found, skipped"; continue }
            $path = Join-Path $inDir "$id.png"
            [IO.File]::WriteAllBytes($path, $png)
            $img = [System.Drawing.Image]::FromFile($path)
            $sizes[$id] = @($img.Width, $img.Height)
            $img.Dispose()
        }
    } finally { [AeroScrollArt]::Close($module) }

    $modelDir = Join-Path (Split-Path $Waifu2x) $Model
    [string[]]$cmd = @('-i', $inDir, '-o', $outDir, '-s', '2', '-n', "$Noise", '-m', $modelDir, '-f', 'png',
                       '-t', "$TileSize")
    if ($Tta) { $cmd = @('-x') + $cmd }
    & $Waifu2x $cmd
    if ($LASTEXITCODE -ne 0) { throw "waifu2x failed with exit code $LASTEXITCODE" }

    foreach ($id in $sizes.Keys) {
        $src = Join-Path $outDir "$id.png"
        if (-not (Test-Path -LiteralPath $src)) { Write-Warning "${id}: no output"; continue }
        $img = [System.Drawing.Image]::FromFile($src)
        $w = $img.Width; $h = $img.Height
        $img.Dispose()
        $ew = 2 * $sizes[$id][0]; $eh = 2 * $sizes[$id][1]
        if ($w -ne $ew -or $h -ne $eh) {
            Write-Warning "${id}: got ${w}x${h}, expected ${ew}x${eh}"; continue
        }
        Copy-Item -LiteralPath $src -Destination (Join-Path $hdDir "$id@2x.png") -Force
        Write-Host "$id -> ${w}x${h}"
    }
    Write-Host "Wrote @2x scrollbar parts to $hdDir; restart foobar2000 to load them."
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
