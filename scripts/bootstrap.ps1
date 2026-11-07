#Requires -Version 7.2
<#
.SYNOPSIS
    Fetch and verify third-party dependencies: the foobar2000 SDK and the portable
    foobar2000 instance used for local development.

.DESCRIPTION
    Versions, URLs and SHA-256 hashes live in third_party/deps.lock.json.
    - The SDK is extracted to third_party/foobar2000-sdk (not committed).
    - The foobar2000 installer is only unpacked with 7-Zip into .local/fb2k-dev and
      marked portable; the installer is never executed and no other foobar2000
      installation on the machine is touched.
    Idempotent; -Force wipes and re-extracts.

.EXAMPLE
    pwsh -File scripts/bootstrap.ps1
#>
[CmdletBinding()]
param(
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Lock = Get-Content -LiteralPath (Join-Path $Root 'third_party/deps.lock.json') -Raw | ConvertFrom-Json
$Cache = Join-Path $Root '.local/cache'
New-Item -ItemType Directory -Force -Path $Cache | Out-Null

function Find-7Zip {
    $candidates = @(
        (Get-Command 7z -ErrorAction SilentlyContinue)?.Source,
        (Join-Path $env:ProgramFiles '7-Zip/7z.exe')
    ) | Where-Object { $_ -and (Test-Path -LiteralPath $_) }
    if (-not $candidates) { throw '7-Zip (7z.exe) is required: wingetx install --id 7zip.7zip --exact --source winget' }
    return $candidates[0]
}

function Get-VerifiedDownload([pscustomobject]$Dep) {
    $file = Join-Path $Cache ([IO.Path]::GetFileName($Dep.url))
    if (-not (Test-Path -LiteralPath $file)) {
        Write-Host "Downloading $($Dep.url)"
        Invoke-WebRequest -Uri $Dep.url -OutFile $file
    }
    $hash = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
    if ($hash -ne $Dep.sha256) {
        Remove-Item -LiteralPath $file
        throw "SHA-256 mismatch for $file`n  expected $($Dep.sha256)`n  actual   $hash"
    }
    if ($Dep.PSObject.Properties.Name -contains 'authenticode_subject') {
        $sig = Get-AuthenticodeSignature -LiteralPath $file
        if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -ne $Dep.authenticode_subject) {
            throw "Signature check failed for $file ($($sig.Status) / $($sig.SignerCertificate.Subject))"
        }
    }
    return $file
}

$7z = Find-7Zip

# --- foobar2000 SDK -----------------------------------------------------------------
$sdk = $Lock.'foobar2000-sdk'
$sdkDest = Join-Path $Root $sdk.dest
$sdkStamp = Join-Path $sdkDest '.version'
if ($Force -and (Test-Path -LiteralPath $sdkDest)) { Remove-Item -LiteralPath $sdkDest -Recurse -Force }
if ((Test-Path -LiteralPath $sdkStamp) -and ((Get-Content -LiteralPath $sdkStamp -Raw).Trim() -eq $sdk.version)) {
    Write-Host "SDK $($sdk.version) is up to date"
} else {
    $archive = Get-VerifiedDownload $sdk
    if (Test-Path -LiteralPath $sdkDest) { Remove-Item -LiteralPath $sdkDest -Recurse -Force }
    & $7z x $archive "-o$sdkDest" -y | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Extracting the SDK failed (7z exit code $LASTEXITCODE)" }
    Set-Content -LiteralPath $sdkStamp -Value $sdk.version
    Write-Host "SDK $($sdk.version) extracted to $sdkDest"
}

# --- Portable foobar2000 dev instance --------------------------------------------------
$fb = $Lock.'foobar2000-dev'
$fbDest = Join-Path $Root $fb.dest
$fbStamp = Join-Path $fbDest '.glasswave-version'
if ($Force -and (Test-Path -LiteralPath $fbDest)) { Remove-Item -LiteralPath $fbDest -Recurse -Force }
if ((Test-Path -LiteralPath $fbStamp) -and ((Get-Content -LiteralPath $fbStamp -Raw).Trim() -eq $fb.version)) {
    Write-Host "foobar2000 $($fb.version) dev instance is up to date"
} else {
    $installer = Get-VerifiedDownload $fb
    $tmp = Join-Path $Cache 'fb2k-extract'
    if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Recurse -Force }
    & $7z x $installer "-o$tmp" -y | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Unpacking the foobar2000 installer failed (7z exit code $LASTEXITCODE)" }
    # NSIS leftovers are not needed.
    foreach ($junk in '$PLUGINSDIR', '$R0', 'uninstall.exe') {
        $p = Join-Path $tmp $junk
        if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Recurse -Force }
    }
    New-Item -ItemType Directory -Force -Path $fbDest | Out-Null
    Copy-Item -Path (Join-Path $tmp '*') -Destination $fbDest -Recurse -Force
    Remove-Item -LiteralPath $tmp -Recurse -Force
    New-Item -ItemType File -Force -Path (Join-Path $fbDest 'portable_mode_enabled') | Out-Null
    Set-Content -LiteralPath $fbStamp -Value $fb.version
    Write-Host "foobar2000 $($fb.version) portable dev instance unpacked to $fbDest"
}
