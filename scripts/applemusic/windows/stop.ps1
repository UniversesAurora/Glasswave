#Requires -Version 7.2

[CmdletBinding()]
param(
    [ValidateRange(1, 65535)]
    [int]$Port = 20025,

    [ValidateRange(1, 30)]
    [int]$TimeoutSeconds = 8
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

Initialize-GlasswavePrivateRuntime
Stop-GlasswaveNativeProcess -Port $Port -TimeoutSeconds $TimeoutSeconds | Out-Null
