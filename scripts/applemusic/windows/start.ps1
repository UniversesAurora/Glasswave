#Requires -Version 7.2

[CmdletBinding()]
param(
    [ValidateRange(1, 65535)]
    [int]$Port = 20025,

    [ValidateRange(1, 180)]
    [int]$TimeoutSeconds = 60
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

$paths = Get-GlasswavePaths
Assert-GlasswaveRuntimeComplete

Initialize-GlasswavePrivateRuntime
if (Test-GlasswaveNativeProcess -Port $Port -Quiet) {
    if (Wait-GlasswaveNativeReady -Port $Port -TimeoutSeconds 5) {
        Write-Host "Native Apple Music helper already running on 127.0.0.1:$Port"
        exit 0
    }
    throw "Native PID record exists for port $Port but the helper is unhealthy."
}
if (Test-Path -LiteralPath (Get-GlasswavePidPath $Port)) {
    Remove-GlasswavePidRecord $Port
}

if (Test-GlasswavePortListening $Port) {
    throw "127.0.0.1:$Port is already in use; no process was stopped."
}

$token = Read-GlasswaveApiToken
$stdout = Join-Path $paths.RuntimeRoot "logs\engine-$Port.stdout.log"
$stderr = Join-Path $paths.RuntimeRoot "logs\engine-$Port.stderr.log"
$environment = @{
    GLASSWAVE_HEADLESS = '1'
    ADAM_API_TOKEN = $token
    ADAM_ADAMPLAY_DIR = (Join-Path $paths.RuntimeRoot 'widevine')
    APPDATA = (Join-Path $paths.RuntimeRoot 'config')
    LOCALAPPDATA = (Join-Path $paths.RuntimeRoot 'localappdata')
    TMP = (Join-Path $paths.RuntimeRoot 'tmp')
    TEMP = (Join-Path $paths.RuntimeRoot 'tmp')
    PATH = "$($paths.RuntimeRoot);$env:PATH"
}
try {
    $process = Start-Process -FilePath $paths.EnginePath `
        -ArgumentList @('--api', "$Port") `
        -WorkingDirectory $paths.RuntimeRoot `
        -WindowStyle Hidden `
        -Environment $environment `
        -RedirectStandardOutput $stdout `
        -RedirectStandardError $stderr `
        -PassThru
}
finally {
    $environment.ADAM_API_TOKEN = $null
    $token = $null
}

$record = [ordered]@{
    enginePid = $process.Id
    enginePath = [IO.Path]::GetFullPath($paths.EnginePath)
    startedUtc = $process.StartTime.ToUniversalTime().ToString('o')
    port = $Port
}
Write-GlasswaveJsonFile -Path (Get-GlasswavePidPath $Port) -Value $record
Set-GlasswavePrivateAcl $stdout
Set-GlasswavePrivateAcl $stderr

Start-Sleep -Milliseconds 500
if ($process.HasExited) {
    Remove-GlasswavePidRecord $Port
    $token = Read-GlasswaveApiToken
    Write-Host "Native Apple Music helper exited during startup (code $($process.ExitCode)). Filtered log tail:"
    Get-Content -LiteralPath $stdout, $stderr -ErrorAction SilentlyContinue |
        Select-Object -Last 160 |
        ForEach-Object { Protect-GlasswaveLogLine -Line $_ -Token $token }
    $token = $null
    exit 1
}

if (-not (Wait-GlasswaveNativeReady -Port $Port -TimeoutSeconds $TimeoutSeconds)) {
    Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    Remove-GlasswavePidRecord $Port
    $token = Read-GlasswaveApiToken
    Write-Host 'Native Apple Music helper failed to become healthy. Filtered log tail:'
    Get-Content -LiteralPath $stdout, $stderr -ErrorAction SilentlyContinue |
        Select-Object -Last 160 |
        ForEach-Object { Protect-GlasswaveLogLine -Line $_ -Token $token }
    $token = $null
    exit 1
}

Write-GlasswaveBridgeConfig $Port
Write-GlasswaveRuntimeManifest
Write-Host "Native Apple Music helper started on 127.0.0.1:$Port (PID $($process.Id))"
