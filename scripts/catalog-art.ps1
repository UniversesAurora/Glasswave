#Requires -Version 7
<#
.SYNOPSIS
    Export every image in the user's Win7 files to .local\catalog for looking things up.

.DESCRIPTION
    Reads wmploc.dll, ehres.dll and aero.msstyles from <profile>\glasswave (ADR 0007: the
    user's own files, never committed) and writes, per module and resource type:
      .local\catalog\<module>\<type>\<name>.png   every image
      .local\catalog\<module>\<type>-sheet-NN.png contact sheets, each cell labelled
      .local\catalog\index.tsv                    module, type, name, width, height, file
    The written index of what is where lives in docs/design/win7-asset-map.md.
#>
[CmdletBinding()]
param(
    [string]$ProfileDir = (Join-Path $PSScriptRoot '..\.local\fb2k-dev\profile'),
    [string]$OutDir = (Join-Path $PSScriptRoot '..\.local\catalog'),
    [int]$PerSheet = 48
)
$ErrorActionPreference = 'Stop'
$art = Join-Path (Resolve-Path $ProfileDir) 'glasswave'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path

Add-Type -AssemblyName System.Drawing
$null = [System.Drawing.Bitmap]
$refs = [AppDomain]::CurrentDomain.GetAssemblies() |
    Where-Object { -not $_.IsDynamic -and $_.Location -and $_.GetName().Name -match '^(System\.Drawing|System\.Private\.Windows|System\.Runtime|netstandard|System\.Collections|System\.Memory|System\.ComponentModel\.Primitives|System\.Runtime\.InteropServices|System\.Private\.CoreLib|System\.Collections\.Generic)' } |
    ForEach-Object Location
$refs = @($refs) + @('System.Collections', 'System.Runtime', 'System.Linq' | ForEach-Object { [Reflection.Assembly]::Load($_).Location })
Add-Type -ReferencedAssemblies ($refs | Select-Object -Unique) -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;

public static class Catalog {
    delegate bool EnumNames(IntPtr m, IntPtr t, IntPtr n, IntPtr p);
    [DllImport("kernel32", CharSet = CharSet.Unicode)] static extern IntPtr LoadLibraryExW(string f, IntPtr h, uint fl);
    [DllImport("kernel32")] static extern bool FreeLibrary(IntPtr m);
    [DllImport("kernel32", CharSet = CharSet.Unicode)] static extern bool EnumResourceNamesW(IntPtr m, IntPtr t, EnumNames cb, IntPtr p);
    [DllImport("kernel32", CharSet = CharSet.Unicode)] static extern IntPtr FindResourceW(IntPtr m, IntPtr n, IntPtr t);
    [DllImport("kernel32")] static extern IntPtr LoadResource(IntPtr m, IntPtr r);
    [DllImport("kernel32")] static extern IntPtr LockResource(IntPtr g);
    [DllImport("kernel32")] static extern uint SizeofResource(IntPtr m, IntPtr r);
    [DllImport("user32", CharSet = CharSet.Unicode)] static extern IntPtr LoadImageW(IntPtr inst, IntPtr name, uint type, int cx, int cy, uint flags);
    [DllImport("user32")] static extern bool DestroyIcon(IntPtr h);

    static string Name(IntPtr p) { return ((long)p >> 16) == 0 ? "#" + (long)p : Marshal.PtrToStringUni(p); }
    static IntPtr Type(string t) { return t.StartsWith("#") ? (IntPtr)int.Parse(t.Substring(1)) : Marshal.StringToHGlobalUni(t); }

    public static IntPtr Open(string f) { return LoadLibraryExW(f, IntPtr.Zero, 0x22); }
    public static void Close(IntPtr m) { FreeLibrary(m); }

    public static string[] Names(IntPtr m, string type) {
        var o = new string[0];
        EnumResourceNamesW(m, Type(type), (a, b, n, d) => { Array.Resize(ref o, o.Length + 1); o[o.Length - 1] = Name(n); return true; }, IntPtr.Zero);
        return o;
    }

    static IntPtr Res(IntPtr m, string name, string type) {
        IntPtr n = name.StartsWith("#") ? (IntPtr)int.Parse(name.Substring(1)) : Marshal.StringToHGlobalUni(name);
        return FindResourceW(m, n, Type(type));
    }

    public static byte[] Bytes(IntPtr m, string name, string type) {
        IntPtr r = Res(m, name, type);
        if (r == IntPtr.Zero) return null;
        uint size = SizeofResource(m, r);
        var b = new byte[size];
        Marshal.Copy(LockResource(LoadResource(m, r)), b, 0, (int)size);
        return b;
    }

    // Decodes PNG / BMP / DIB (RT_BITMAP has no file header) / ICO-group into a bitmap, or null.
    public static Bitmap Decode(IntPtr m, string name, string type) {
        if (type == "#14") {  // icon group: let USER pick the largest frame
            IntPtr n = name.StartsWith("#") ? (IntPtr)int.Parse(name.Substring(1)) : Marshal.StringToHGlobalUni(name);
            IntPtr h = LoadImageW(m, n, 1, 256, 256, 0);
            if (h == IntPtr.Zero) h = LoadImageW(m, n, 1, 48, 48, 0);
            if (h == IntPtr.Zero) return null;
            using var ic = Icon.FromHandle(h);
            var bmp = ic.ToBitmap();
            DestroyIcon(h);
            return bmp;
        }
        byte[] b = Bytes(m, name, type);
        if (b == null || b.Length < 8) return null;
        try {
            if (b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') return new Bitmap(new MemoryStream(b));
            if (b[0] == 'B' && b[1] == 'M') return new Bitmap(new MemoryStream(b));
            if (b[0] == 0xFF && b[1] == 0xD8) return new Bitmap(new MemoryStream(b));
            if (b[0] == 'G' && b[1] == 'I' && b[2] == 'F') return new Bitmap(new MemoryStream(b));
            if (type == "#2" && BitConverter.ToInt32(b, 0) == 40) {  // DIB: prepend a file header
                int bpp = BitConverter.ToInt16(b, 14), clr = BitConverter.ToInt32(b, 32);
                int pal = bpp <= 8 ? (clr == 0 ? 1 << bpp : clr) * 4 : 0;
                int compression = BitConverter.ToInt32(b, 16);
                if (compression == 3) pal = 12;
                var f = new byte[14 + b.Length];
                f[0] = (byte)'B'; f[1] = (byte)'M';
                BitConverter.GetBytes(f.Length).CopyTo(f, 2);
                BitConverter.GetBytes(14 + 40 + pal).CopyTo(f, 10);
                b.CopyTo(f, 14);
                return new Bitmap(new MemoryStream(f));
            }
        } catch { }
        return null;
    }

    public static void Sheet(string[] files, string[] labels, string output) {
        const int cell = 160, label = 16, cols = 8;
        int rows = (files.Length + cols - 1) / cols;
        using var sheet = new Bitmap(cols * cell, rows * (cell + label));
        using var g = Graphics.FromImage(sheet);
        g.Clear(Color.FromArgb(255, 200, 214, 230));
        using var checker = new SolidBrush(Color.FromArgb(255, 170, 186, 204));
        using var font = new Font("Arial", 9);
        g.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.NearestNeighbor;
        g.PixelOffsetMode = System.Drawing.Drawing2D.PixelOffsetMode.Half;
        for (int i = 0; i < files.Length; ++i) {
            int x = (i % cols) * cell, y = (i / cols) * (cell + label);
            for (int cy = 0; cy < cell - 4; cy += 8)
                for (int cx = ((cy / 8) % 2) * 8; cx < cell - 4; cx += 16) g.FillRectangle(checker, x + 2 + cx, y + label + 2 + cy, 8, 8);
            using var img = new Bitmap(files[i]);
            float k = Math.Min(1f * (cell - 6) / img.Width, 1f * (cell - 6) / img.Height);
            if (k > 4) k = (float)Math.Floor(k > 4 ? 4 : k);
            else if (k >= 1) k = (float)Math.Floor(k);
            int w = Math.Max(1, (int)(img.Width * k)), h = Math.Max(1, (int)(img.Height * k));
            g.DrawImage(img, x + 3, y + label + 3, w, h);
            g.DrawString(labels[i], font, Brushes.Black, x + 2, y + 1);
        }
        sheet.Save(output, ImageFormat.Png);
    }
}
'@

$modules = @(
    @{ Name = 'wmploc'; File = 'wmploc.dll'; Types = @('#257', '#256', '#2', '#14') },
    @{ Name = 'aero'; File = 'aero.msstyles'; Types = @('IMAGE') },
    @{ Name = 'ehres'; File = 'ehres.dll'; Types = @('#10', '#14') }
)
$index = [System.Collections.Generic.List[string]]::new()
$index.Add("module`ttype`tname`twidth`theight`tfile")
foreach ($mod in $modules) {
    $path = Join-Path $art $mod.File
    if (-not (Test-Path -LiteralPath $path)) { Write-Warning "$($mod.File) missing, skipped"; continue }
    $h = [Catalog]::Open($path)
    try {
        foreach ($type in $mod.Types) {
            $tdir = Join-Path $OutDir "$($mod.Name)\$($type.TrimStart('#'))"
            New-Item -ItemType Directory -Force -Path $tdir | Out-Null
            $files = [System.Collections.Generic.List[string]]::new()
            $labels = [System.Collections.Generic.List[string]]::new()
            $names = [Catalog]::Names($h, $type) | Sort-Object { if ($_ -match '^#(\d+)$') { [int]$Matches[1] } else { [int]::MaxValue } }, { $_ }
            foreach ($n in $names) {
                $bmp = [Catalog]::Decode($h, $n, $type)
                if (-not $bmp) { continue }
                $file = Join-Path $tdir (($n.TrimStart('#') -replace '[\\/:*?"<>|]', '_') + '.png')
                $bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png)
                $index.Add("$($mod.Name)`t$type`t$n`t$($bmp.Width)`t$($bmp.Height)`t$file")
                $files.Add($file); $labels.Add("$($n.TrimStart('#')) $($bmp.Width)x$($bmp.Height)")
                $bmp.Dispose()
            }
            for ($i = 0; $i -lt $files.Count; $i += $PerSheet) {
                $count = [Math]::Min($PerSheet, $files.Count - $i)
                $sheet = Join-Path $OutDir ('{0}\{1}-sheet-{2:D2}.png' -f $mod.Name, $type.TrimStart('#'), [int]($i / $PerSheet))
                [Catalog]::Sheet([string[]]$files.GetRange($i, $count).ToArray(), [string[]]$labels.GetRange($i, $count).ToArray(), $sheet)
            }
            Write-Host ("{0} {1}: {2} images" -f $mod.Name, $type, $files.Count)
        }
    } finally { [Catalog]::Close($h) }
}
[IO.File]::WriteAllLines((Join-Path $OutDir 'index.tsv'), $index)
Write-Host "Index: $(Join-Path $OutDir 'index.tsv')"
