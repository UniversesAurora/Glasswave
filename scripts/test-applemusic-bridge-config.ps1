#Requires -Version 7.2

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$testRoot = Join-Path ([IO.Path]::GetTempPath()) "glasswave-bridge-$([guid]::NewGuid().ToString('N'))"

try {
    $testCommon = Join-Path $testRoot 'scripts/applemusic/windows/common.ps1'
    $tokenPath = Join-Path $testRoot '.local/applemusic/api-token.txt'
    $bridgePath = Join-Path $testRoot '.local/fb2k-dev/profile/glasswave/applemusic/bridge.json'
    $runtimeDirectory = Join-Path $testRoot '.local/applemusic/windows/runtime'
    New-Item -ItemType Directory -Force -Path (Split-Path $tokenPath -Parent), $runtimeDirectory |
        Out-Null
    New-Item -ItemType Directory -Force -Path (Split-Path $testCommon -Parent) | Out-Null
    Copy-Item -LiteralPath (Join-Path $root 'scripts/applemusic/windows/common.ps1') `
        -Destination $testCommon

    . $testCommon

    Initialize-GlasswaveApiToken
    $token = Read-GlasswaveApiToken
    Write-GlasswaveBridgeConfig -Port 20025
    $config = Get-Content -LiteralPath $bridgePath -Raw | ConvertFrom-Json -ErrorAction Stop
    $writtenRuntime = [IO.Path]::GetFullPath([string]$config.managed.runtimeDirectory)
    $expectedRuntime = [IO.Path]::GetFullPath($runtimeDirectory)

    if ($config.url -ne 'http://127.0.0.1:20025') {
        throw 'Bridge URL contract failed.'
    }
    if (-not [string]::Equals([string]$config.token, $token, [StringComparison]::Ordinal)) {
        throw 'Bridge token contract failed.'
    }
    if (-not [IO.Path]::IsPathFullyQualified([string]$config.managed.runtimeDirectory)) {
        throw 'Managed runtime directory is not absolute.'
    }
    if (-not [string]::Equals($writtenRuntime, $expectedRuntime, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Managed runtime directory contract failed.'
    }
    if (Test-Path -LiteralPath (Join-Path $runtimeDirectory 'engine.exe')) {
        throw 'The test unexpectedly contains a live runtime executable.'
    }
    if (-not (Test-GlasswaveBridgeConfig -Port 20025)) {
        throw 'Bridge verification did not accept the generated descriptor.'
    }

    Write-Host 'Apple Music bridge metadata contract: PASS'
}
finally {
    $token = $null
    Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
}
