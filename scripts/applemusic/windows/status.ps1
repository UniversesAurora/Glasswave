#Requires -Version 7.2

[CmdletBinding()]
param(
    [ValidateRange(1, 65535)]
    [int]$Port = 20025
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

$record = Read-GlasswavePidRecord $Port
$owned = Test-GlasswaveNativeProcess -Port $Port -Quiet
$healthy = $false
$version = $null
if ($owned) {
    try {
        $response = Invoke-GlasswaveApi -Port $Port -Path '/api/v1/status' -TimeoutSeconds 3
        if ($response.StatusCode -eq 200) {
            $healthy = $true
            try {
                $version = ($response.Body | ConvertFrom-Json -ErrorAction Stop).version
            }
            catch {
            }
        }
    }
    catch {
    }
}
$bridgeMatches = Test-GlasswaveBridgeConfig $Port
$runtimePresent = Test-GlasswaveRuntimeComplete
$status = if ($healthy) { 'running' } elseif ($owned) { 'unhealthy' } else { 'stopped' }
$pidValue = if ($null -ne $record) { [int]$record.enginePid } else { $null }

[pscustomobject]@{
    status = $status
    port = $Port
    url = "http://127.0.0.1:$Port"
    pid = $pidValue
    owned = $owned
    healthy = $healthy
    version = $version
    bridgeMatches = $bridgeMatches
    runtimePresent = $runtimePresent
}

if ($status -eq 'running' -and -not $bridgeMatches) {
    Write-Host 'Dev bridge config: missing or stale'
}
if ($status -ne 'running') {
    exit 1
}
