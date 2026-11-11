#Requires -Version 7.2

[CmdletBinding()]
param(
    [ValidateRange(1, 65535)]
    [int]$Port = 20025,

    [ValidateRange(1, 180)]
    [int]$TimeoutSeconds = 60
)

$ErrorActionPreference = 'Stop'

& (Join-Path $PSScriptRoot 'stop.ps1') -Port $Port
if (-not $?) {
    throw "Unable to stop the native Apple Music helper on port $Port."
}
& (Join-Path $PSScriptRoot 'start.ps1') -Port $Port -TimeoutSeconds $TimeoutSeconds
if (-not $?) {
    throw "Unable to start the native Apple Music helper on port $Port."
}
