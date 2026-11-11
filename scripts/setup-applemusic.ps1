#Requires -Version 7.2

[CmdletBinding()]
param(
    [ValidateSet('Check', 'Prepare', 'Build', 'Start', 'Restart', 'Stop', 'Status', 'Manifest')]
    [string]$Action = 'Check',

    [ValidateRange(1, 65535)]
    [int]$Port = 20025,

    [switch]$PromoteRuntime
)

$ErrorActionPreference = 'Stop'
$windowsScripts = Join-Path $PSScriptRoot 'applemusic\windows'
. (Join-Path $windowsScripts 'common.ps1')

function Initialize-ApiToken {
    $paths = Get-GlasswavePaths
    if (Test-Path -LiteralPath $paths.TokenPath -PathType Leaf) {
        Read-GlasswaveApiToken | Out-Null
        return
    }
    $token = [Convert]::ToHexString(
        [Security.Cryptography.RandomNumberGenerator]::GetBytes(32)
    )
    New-Item -ItemType Directory -Force -Path (Split-Path $paths.TokenPath -Parent) | Out-Null
    [IO.File]::WriteAllText($paths.TokenPath, $token, [Text.UTF8Encoding]::new($false))
    Set-GlasswavePrivateAcl $paths.TokenPath
    $token = $null
}

function Invoke-WindowsStageScript {
    param(
        [Parameter(Mandatory)][string]$Name,
        [hashtable]$Arguments = @{}
    )
    & (Join-Path $windowsScripts $Name) @Arguments
    if (-not $?) {
        throw "$Name failed."
    }
}

switch ($Action) {
    'Check' {
        Invoke-WindowsStageScript 'check.ps1' @{ Port = $Port }
    }
    'Prepare' {
        $mise = Get-Command mise.exe -ErrorAction Stop
        & $mise.Source install 'go@1.26.0'
        if ($LASTEXITCODE -ne 0) {
            throw 'mise could not install Go 1.26.0.'
        }
        Initialize-GlasswavePrivateRuntime
        Initialize-ApiToken
        Invoke-WindowsStageScript 'check.ps1' @{ Port = $Port }
    }
    'Build' {
        Initialize-GlasswavePrivateRuntime
        Initialize-ApiToken
        Invoke-WindowsStageScript 'build.ps1' @{ PromoteRuntime = $PromoteRuntime }
    }
    'Start' {
        Initialize-GlasswavePrivateRuntime
        Initialize-ApiToken
        Invoke-WindowsStageScript 'start.ps1' @{ Port = $Port }
    }
    'Restart' {
        Initialize-ApiToken
        Invoke-WindowsStageScript 'restart.ps1' @{ Port = $Port }
    }
    'Stop' {
        Invoke-WindowsStageScript 'stop.ps1' @{ Port = $Port }
    }
    'Status' {
        Invoke-WindowsStageScript 'status.ps1' @{ Port = $Port }
    }
    'Manifest' {
        Write-GlasswaveRuntimeManifest
        Write-Host (Get-GlasswavePaths).RuntimeManifestPath
    }
}
