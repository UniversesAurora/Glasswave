#Requires -Version 7.2
<#
.SYNOPSIS
    Deploy the built component into the .local/fb2k-dev portable instance and start it.

.DESCRIPTION
    Only touches the repository's own dev instance (.local/fb2k-dev), never any other
    foobar2000 installation on the machine. A running dev instance is asked to close
    first (-Kill forces it).

.EXAMPLE
    pwsh -File scripts/run-dev.ps1               # deploy Release|x64 and start
    pwsh -File scripts/run-dev.ps1 -NoLaunch     # deploy only
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')] [string]$Configuration = 'Release',
    [switch]$NoLaunch,
    [switch]$Kill
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Instance = Join-Path $Root '.local/fb2k-dev'
$Exe = Join-Path $Instance 'foobar2000.exe'
if (-not (Test-Path -LiteralPath $Exe)) { throw 'Dev instance missing - run scripts/bootstrap.ps1 first' }

$built = Join-Path $Root "build/x64/$Configuration"
$dll = Join-Path $built 'foo_ui_glasswave.dll'
if (-not (Test-Path -LiteralPath $dll)) { throw "Missing $dll - run scripts/build.ps1 -Configuration $Configuration" }

# Only processes whose image is this instance's foobar2000.exe.
$procs = Get-Process foobar2000 -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq (Resolve-Path -LiteralPath $Exe).Path }
foreach ($p in $procs) {
    if ($Kill) { $p.Kill() } else { [void]$p.CloseMainWindow() }
    if (-not $p.WaitForExit(8000)) { $p.Kill(); [void]$p.WaitForExit(3000) }
}

# foobar2000 2.x portable mode: 64-bit components live in profile/user-components-x64/<name>/
$dest = Join-Path $Instance 'profile/user-components-x64/foo_ui_glasswave'
New-Item -ItemType Directory -Force -Path $dest | Out-Null
Copy-Item -LiteralPath $dll -Destination $dest -Force
$pdb = Join-Path $built 'foo_ui_glasswave.pdb'
if (Test-Path -LiteralPath $pdb) { Copy-Item -LiteralPath $pdb -Destination $dest -Force }
Write-Host "Deployed $Configuration build to $dest"

# Original Win7 artwork (ADR 0007): never committed; copied from the owner's local
# references into the dev profile, where Glasswave looks for it at run time.
$art = Join-Path $Instance 'profile/glasswave'
$sources = @{
    'wmploc.dll'    = '.local/refs/wmploc.dll'
    'ehres.dll'     = '.local/refs/ehres.dll'
    'aero.msstyles' = '.local/refs/Aero/aero.msstyles'
}
foreach ($name in $sources.Keys) {
    $src = Join-Path $Root $sources[$name]
    if (-not (Test-Path -LiteralPath $src)) { continue }
    New-Item -ItemType Directory -Force -Path $art | Out-Null
    $dst = Join-Path $art $name
    if (-not (Test-Path -LiteralPath $dst) -or (Get-Item -LiteralPath $src).Length -ne (Get-Item -LiteralPath $dst).Length) {
        Copy-Item -LiteralPath $src -Destination $dst -Force
        Write-Host "Copied Win7 artwork $name"
    }
}

if (-not $NoLaunch) {
    Start-Process -FilePath $Exe -WorkingDirectory $Instance
    Write-Host 'Dev instance started'
}
