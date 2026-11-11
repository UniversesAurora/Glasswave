#Requires -Version 7.2

[CmdletBinding()]
param(
    [ValidateRange(1, 65535)]
    [int]$Port = 20025,

    [switch]$SkipFairPlayCheck
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

$arguments = @{ CheckOnly = $true }
if ($SkipFairPlayCheck) {
    $arguments.SkipFairPlayCheck = $true
}
& (Join-Path $PSScriptRoot 'build.ps1') @arguments
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

Assert-GlasswaveRuntimeComplete

$bridge = if (Test-GlasswaveBridgeConfig $Port) { 'matches' } else { 'missing or stale' }
Write-Host "Native Apple Music runtime: ready"
Write-Host "Dev bridge config for port ${Port}: $bridge"
