#Requires -Version 7.2
<#
.SYNOPSIS
    Run the bounded Apple Music app-managed lifecycle regression.

.DESCRIPTION
    DEV DIAGNOSTIC ONLY. This starts and gracefully stops only the repository's
    .local/fb2k-dev foobar2000.exe. It uses the existing bridge, runtime and preserved
    account session; it does not build, deploy, install, configure, mock or write Apple
    data. The bridge token is used only in request headers and is never printed.

    The default run checks direct cold start, authenticated account access, normal quit,
    login retention on a second direct start, and forced-parent Job cleanup. It leaves
    the dev instance running. -IncludeExternalPreservation additionally checks that the
    component borrows an engine started by scripts/applemusic/windows/start.ps1 and does
    not stop it when the player exits.

.EXAMPLE
    pwsh -File scripts/test-applemusic-lifecycle.ps1

.EXAMPLE
    pwsh -File scripts/test-applemusic-lifecycle.ps1 -IncludeExternalPreservation
#>
[CmdletBinding()]
param(
    [ValidateRange(10, 180)]
    [int]$StartupTimeoutSeconds = 60,

    [ValidateRange(2, 30)]
    [int]$QuitTimeoutSeconds = 12,

    [switch]$IncludeExternalPreservation
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$Root = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$DevRoot = [IO.Path]::GetFullPath((Join-Path $Root '.local/fb2k-dev'))
$DevExe = [IO.Path]::GetFullPath((Join-Path $DevRoot 'foobar2000.exe'))
$BridgePath = [IO.Path]::GetFullPath(
    (Join-Path $DevRoot 'profile/glasswave/applemusic/bridge.json'))
$AppleScripts = Join-Path $Root 'scripts/applemusic/windows'
$StartScript = Join-Path $AppleScripts 'start.ps1'
$StopScript = Join-Path $AppleScripts 'stop.ps1'

foreach ($required in @($DevExe, $BridgePath, $StartScript, $StopScript)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required dev diagnostic file is missing: $required"
    }
}

function Test-SamePath {
    param([string]$Left, [string]$Right)
    return [string]::Equals(
        [IO.Path]::GetFullPath($Left),
        [IO.Path]::GetFullPath($Right),
        [StringComparison]::OrdinalIgnoreCase)
}

function Get-ExactProcess {
    param([int]$Id, [string]$Path)

    $process = Get-Process -Id $Id -ErrorAction SilentlyContinue
    if ($null -eq $process -or $process.HasExited) {
        return $null
    }
    try {
        if (Test-SamePath $process.Path $Path) {
            return $process
        }
    }
    catch {
    }
    return $null
}

function Get-ExactDevPlayers {
    @(
        Get-CimInstance Win32_Process -Filter "Name = 'foobar2000.exe'" -ErrorAction SilentlyContinue |
            Where-Object {
                $_.ExecutablePath -and
                (Test-SamePath $_.ExecutablePath $DevExe)
            } |
            ForEach-Object { Get-ExactProcess -Id ([int]$_.ProcessId) -Path $DevExe }
    )
}

function Get-ExactEngines {
    @(
        Get-CimInstance Win32_Process -Filter "Name = 'engine.exe'" -ErrorAction SilentlyContinue |
            Where-Object {
                $_.ExecutablePath -and
                (Test-SamePath $_.ExecutablePath $script:EngineExe)
            } |
            ForEach-Object { Get-ExactProcess -Id ([int]$_.ProcessId) -Path $script:EngineExe }
    )
}

function Test-ProcessIdentity {
    param(
        [System.Diagnostics.Process]$Process,
        [string]$Path,
        [DateTime]$StartedUtc
    )

    $current = Get-ExactProcess -Id $Process.Id -Path $Path
    if ($null -eq $current) {
        return $false
    }
    return [math]::Abs(($current.StartTime.ToUniversalTime() - $StartedUtc).TotalSeconds) -le 2
}

function Invoke-AuthenticatedJson {
    param([string]$Path)

    $headers = @{ 'X-API-Token' = $script:BridgeToken }
    $response = Invoke-WebRequest -Uri ($script:BridgeUrl + $Path) -Headers $headers `
        -NoProxy -TimeoutSec 8 -SkipHttpErrorCheck -ErrorAction Stop
    if ([int]$response.StatusCode -ne 200) {
        throw "Authenticated $Path returned HTTP $($response.StatusCode)."
    }
    try {
        $body = $response.Content | ConvertFrom-Json -ErrorAction Stop
    }
    catch {
        throw "Authenticated $Path returned invalid JSON."
    }
    if ($body.PSObject.Properties['error'] -and
        -not [string]::IsNullOrEmpty([string]$body.error)) {
        throw "Authenticated $Path reported an error."
    }
    if ($body.PSObject.Properties['errors'] -and @($body.errors).Count -ne 0) {
        throw "Authenticated $Path returned errors."
    }
    return $body
}

function Get-PidRecord {
    $path = Join-Path $script:RuntimeRoot "engine-$($script:Port).pid.json"
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        return $null
    }
    try {
        return Get-Content -LiteralPath $path -Raw | ConvertFrom-Json -ErrorAction Stop
    }
    catch {
        return 'invalid'
    }
}

function Assert-EngineOwnership {
    param(
        [pscustomobject]$Player,
        [string]$Phase
    )

    $record = Get-PidRecord
    if ($null -eq $record) {
        throw "$Phase did not write the app-managed PID record."
    }
    if ($record -is [string]) {
        throw "$Phase wrote an invalid PID record."
    }
    foreach ($name in 'enginePid', 'enginePath', 'startedUtc', 'port', 'ownerPid') {
        if ($null -eq $record.PSObject.Properties[$name]) {
            throw "$Phase PID record is missing $name."
        }
    }
    if ([int]$record.port -ne $script:Port) {
        throw "$Phase PID record has the wrong port."
    }
    if ([int]$record.ownerPid -ne $Player.Process.Id) {
        throw "$Phase PID record owner does not match the direct player process."
    }
    if (-not (Test-SamePath ([string]$record.enginePath) $script:EngineExe)) {
        throw "$Phase PID record has the wrong executable path."
    }

    $engine = Get-ExactProcess -Id ([int]$record.enginePid) -Path $script:EngineExe
    if ($null -eq $engine) {
        throw "$Phase did not leave its exact engine executable running."
    }
    $recordStart = if ($record.startedUtc -is [DateTime]) {
        $record.startedUtc.ToUniversalTime()
    }
    else {
        [DateTimeOffset]::Parse([string]$record.startedUtc).UtcDateTime
    }
    if ([math]::Abs(($engine.StartTime.ToUniversalTime() - $recordStart).TotalSeconds) -gt 2) {
        throw "$Phase engine start time does not match the PID record."
    }

    $parent = Get-CimInstance Win32_Process -Filter "ProcessId = $($engine.Id)" -ErrorAction Stop
    if ([int]$parent.ParentProcessId -ne $Player.Process.Id) {
        throw "$Phase engine parent is not the direct player process."
    }
    $listeners = @(
        Get-NetTCPConnection -State Listen -LocalPort $script:Port -ErrorAction Stop |
            Where-Object {
                $_.LocalAddress -eq '127.0.0.1' -and
                [int]$_.OwningProcess -eq $engine.Id
            }
    )
    if ($listeners.Count -eq 0) {
        throw "$Phase engine does not own 127.0.0.1:$($script:Port)."
    }

    return [pscustomobject]@{
        Process = $engine
        StartedUtc = $engine.StartTime.ToUniversalTime()
        Record = $record
    }
}

function Start-DevPlayer {
    param([string]$Phase)

    $process = Start-Process -FilePath $DevExe -WorkingDirectory $DevRoot -PassThru
    if ($null -eq $process) {
        throw "$Phase could not start the direct player process."
    }
    # An external bridge may already answer before the player has created its window.
    # Do not attempt a normal-close assertion against that unrelated readiness signal.
    $windowDeadline = [DateTime]::UtcNow.AddSeconds(15)
    do {
        $process.Refresh()
        if ($process.HasExited) { throw "$Phase player exited during window creation." }
        if ($process.MainWindowHandle -ne [IntPtr]::Zero) { break }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $windowDeadline)
    if ($process.MainWindowHandle -eq [IntPtr]::Zero) {
        throw "$Phase player did not create its main window."
    }
    $player = [pscustomobject]@{
        Process = $process
        StartedUtc = $process.StartTime.ToUniversalTime()
    }
    $script:CurrentPlayer = $player
    Write-Host "$Phase player: PID $($process.Id), $DevExe"
    return $player
}

function Wait-ManagedSession {
    param(
        [pscustomobject]$Player,
        [string]$Phase
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($StartupTimeoutSeconds)
    $lastError = 'the bridge has not started'
    $status = $null
    while ([DateTime]::UtcNow -lt $deadline) {
        if (-not (Test-ProcessIdentity $Player.Process $DevExe $Player.StartedUtc)) {
            throw "$Phase player exited before the authenticated bridge became ready."
        }
        try {
            $status = Invoke-AuthenticatedJson '/api/v1/status'
            break
        }
        catch {
            $lastError = $_.Exception.Message
            if ($null -ne (Get-PidRecord) -and @(Get-NetTCPConnection -State Listen `
                    -LocalPort $script:Port -ErrorAction SilentlyContinue).Count -ne 0) {
                throw "$Phase authenticated /api/v1/status failed: $lastError"
            }
        }
        Start-Sleep -Milliseconds 250
    }
    if ($null -eq $status) {
        throw "$Phase did not become ready within $StartupTimeoutSeconds seconds: $lastError"
    }

    $engine = Assert-EngineOwnership $Player $Phase
    $account = Invoke-AuthenticatedJson '/api/v1/glasswave/account'
    if ($account.signedIn -ne $true) {
        throw "$Phase authenticated account is not signed in."
    }
    Write-Host "$Phase status: GET /api/v1/status HTTP 200"
    Write-Host "$Phase account: GET /api/v1/glasswave/account signedIn=true"
    Write-Host "$Phase engine ownership: PID $($engine.Process.Id), parent/ownerPID $($Player.Process.Id)"

    return [pscustomobject]@{
        Player = $Player
        Engine = $engine
    }
}

function Close-DirectPlayer {
    param(
        [pscustomobject]$Player,
        [switch]$AllowForce
    )

    if (-not (Test-ProcessIdentity $Player.Process $DevExe $Player.StartedUtc)) {
        return
    }
    [void]$Player.Process.CloseMainWindow()
    if ($Player.Process.WaitForExit($QuitTimeoutSeconds * 1000)) {
        return
    }
    if (-not $AllowForce) {
        throw "Direct player PID $($Player.Process.Id) did not quit within $QuitTimeoutSeconds seconds."
    }
    if (-not (Test-ProcessIdentity $Player.Process $DevExe $Player.StartedUtc)) {
        throw "Refusing to force an unverified dev player identity."
    }
    Stop-Process -Id $Player.Process.Id -Force -ErrorAction Stop
    if (-not $Player.Process.WaitForExit(3000)) {
        throw "Direct player PID $($Player.Process.Id) did not exit after exact-path cleanup."
    }
    Write-Host "Cleanup stopped verified dev player PID $($Player.Process.Id)."
}

function Wait-ProcessGone {
    param(
        [pscustomobject]$Identity,
        [string]$Phase
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($QuitTimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (-not (Test-ProcessIdentity $Identity.Process $script:EngineExe $Identity.StartedUtc)) {
            return
        }
        Start-Sleep -Milliseconds 150
    }
    throw "$Phase left engine PID $($Identity.Process.Id) running."
}

function Wait-ListenerGone {
    param([string]$Phase)

    $deadline = [DateTime]::UtcNow.AddSeconds($QuitTimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $listeners = @(Get-NetTCPConnection -State Listen -LocalPort $script:Port `
            -ErrorAction SilentlyContinue)
        if ($listeners.Count -eq 0) {
            return
        }
        Start-Sleep -Milliseconds 150
    }
    throw "$Phase left 127.0.0.1:$($script:Port) listening."
}

function Assert-AuthenticatedAccount {
    param([string]$Phase)

    $deadline = [DateTime]::UtcNow.AddSeconds($StartupTimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        try {
            [void](Invoke-AuthenticatedJson '/api/v1/status')
            $account = Invoke-AuthenticatedJson '/api/v1/glasswave/account'
            if ($account.signedIn -ne $true) {
                throw "$Phase account is not signed in."
            }
            Write-Host "$Phase status: GET /api/v1/status HTTP 200"
            Write-Host "$Phase account: GET /api/v1/glasswave/account signedIn=true"
            return
        }
        catch {
            if ($_.Exception.Message -like '*account is not signed in*') {
                throw
            }
            Start-Sleep -Milliseconds 250
        }
    }
    throw "$Phase did not return an authenticated account within $StartupTimeoutSeconds seconds."
}

$script:MayChangeLiveState = $false
$script:CurrentPlayer = $null
$script:ExternalLifecycle = $false
$primaryError = $null

try {
    $bridgeItem = Get-Item -LiteralPath $BridgePath -Force
    if (($bridgeItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw 'The dev bridge must be a regular file, not a reparse point.'
    }
    $bridge = Get-Content -LiteralPath $BridgePath -Raw | ConvertFrom-Json -ErrorAction Stop
    $urlMatch = [regex]::Match([string]$bridge.url, '^http://127\.0\.0\.1:(\d{1,5})$')
    if (-not $urlMatch.Success) {
        throw 'The dev bridge URL must be explicit http://127.0.0.1:<port>.'
    }
    $script:Port = [int]$urlMatch.Groups[1].Value
    if ($script:Port -lt 1 -or $script:Port -gt 65535) {
        throw 'The dev bridge port is invalid.'
    }
    if ($null -eq $bridge.PSObject.Properties['token'] -or
        [string]::IsNullOrWhiteSpace([string]$bridge.token) -or
        ([string]$bridge.token).Length -gt 16384 -or
        [string]$bridge.token -match '[\x00-\x1F\x7F]') {
        throw 'The dev bridge token is missing or invalid.'
    }
    if ($null -eq $bridge.PSObject.Properties['managed'] -or $null -eq $bridge.managed -or
        $null -eq $bridge.managed.PSObject.Properties['runtimeDirectory']) {
        throw 'The dev bridge does not select an app-managed runtime; this script does not configure it.'
    }
    $script:RuntimeRoot = [IO.Path]::GetFullPath([string]$bridge.managed.runtimeDirectory)
    $script:EngineExe = [IO.Path]::GetFullPath((Join-Path $script:RuntimeRoot 'engine.exe'))
    if (-not (Test-Path -LiteralPath $script:EngineExe -PathType Leaf)) {
        throw "The app-managed engine is missing: $script:EngineExe"
    }
    $script:BridgeUrl = 'http://127.0.0.1:' + $script:Port
    $script:BridgeToken = [string]$bridge.token
    $bridge = $null

    Write-Host 'Apple Music lifecycle regression: dev diagnostic invocation'
    Write-Host "Scope: $DevRoot"
    Write-Host "App-managed engine: $script:EngineExe"
    Write-Host "Bridge: $script:BridgeUrl (token verified; value hidden)"

    $script:MayChangeLiveState = $true
    foreach ($existing in @(Get-ExactDevPlayers)) {
        $identity = [pscustomobject]@{
            Process = $existing
            StartedUtc = $existing.StartTime.ToUniversalTime()
        }
        Close-DirectPlayer $identity -AllowForce
        Write-Host "Existing dev player PID $($existing.Id) closed."
    }
    $script:MayChangeLiveState = $true
    $deadline = [DateTime]::UtcNow.AddSeconds($QuitTimeoutSeconds)
    while (@(Get-ExactEngines).Count -ne 0 -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 150
    }
    if (@(Get-ExactEngines).Count -ne 0) {
        throw 'An exact app-managed engine was already running outside this test; no process was stopped.'
    }
    Wait-ListenerGone 'Preflight'

    $coldPlayer = Start-DevPlayer 'Cold direct start'
    $cold = Wait-ManagedSession $coldPlayer 'Cold direct start'
    $coldEnginePid = $cold.Engine.Process.Id

    [void]$cold.Player.Process.CloseMainWindow()
    if (-not $cold.Player.Process.WaitForExit($QuitTimeoutSeconds * 1000)) {
        throw 'Normal quit did not exit the direct player process.'
    }
    Wait-ProcessGone $cold.Engine 'Normal quit'
    Wait-ListenerGone 'Normal quit'
    $script:CurrentPlayer = $null
    Write-Host "Normal quit: engine PID $coldEnginePid exited; listener 127.0.0.1:$($script:Port) closed"

    $restartPlayer = Start-DevPlayer 'Second direct start'
    $restart = Wait-ManagedSession $restartPlayer 'Second direct start'
    if ($restart.Engine.Process.Id -eq $coldEnginePid) {
        throw 'Second direct start reused the exited engine PID.'
    }
    Write-Host 'Second direct start: preserved account session remains signedIn=true'

    if (-not (Test-ProcessIdentity $restart.Player.Process $DevExe $restart.Player.StartedUtc)) {
        throw 'Refusing to force an unverified second-start player identity.'
    }
    Stop-Process -Id $restart.Player.Process.Id -Force -ErrorAction Stop
    if (-not $restart.Player.Process.WaitForExit(3000)) {
        throw 'Forced parent exit did not terminate the direct player process.'
    }
    Wait-ProcessGone $restart.Engine 'Forced parent exit'
    Wait-ListenerGone 'Forced parent exit'
    $script:CurrentPlayer = $null
    $recordRemains = Test-Path -LiteralPath (Join-Path $script:RuntimeRoot "engine-$($script:Port).pid.json")
    Write-Host "Forced parent exit: engine PID $($restart.Engine.Process.Id) exited; listener closed"
    Write-Host "Forced parent exit: PID record present=$recordRemains (record state is not a cleanup assertion)"

    if ($IncludeExternalPreservation) {
        Write-Host 'External service preservation: starting exact owned external engine'
        $script:ExternalLifecycle = $true
        & $StartScript -Port $script:Port -TimeoutSeconds $StartupTimeoutSeconds
        if (-not $?) {
            throw 'The existing external start script failed.'
        }
        $externalRecord = Get-PidRecord
        if ($null -eq $externalRecord -or $externalRecord -is [string]) {
            throw 'The external start script did not write a valid PID record.'
        }
        $external = Get-ExactProcess -Id ([int]$externalRecord.enginePid) -Path $script:EngineExe
        if ($null -eq $external) {
            throw 'The external start script did not leave the exact engine running.'
        }
        $externalIdentity = [pscustomobject]@{
            Process = $external
            StartedUtc = $external.StartTime.ToUniversalTime()
        }
        $externalPid = $external.Id

        $borrowPlayer = Start-DevPlayer 'External borrow'
        Assert-AuthenticatedAccount 'External borrow'
        $exactEngines = @(Get-ExactEngines)
        if ($exactEngines.Count -ne 1 -or [int]$exactEngines[0].Id -ne $externalPid) {
            throw 'External borrow did not preserve the single external engine.'
        }
        [void]$borrowPlayer.Process.CloseMainWindow()
        if (-not $borrowPlayer.Process.WaitForExit($QuitTimeoutSeconds * 1000)) {
            throw 'External-borrow player did not quit normally.'
        }
        if (-not (Test-ProcessIdentity $external $script:EngineExe $externalIdentity.StartedUtc)) {
            throw 'Player quit stopped the externally owned engine.'
        }
        $script:CurrentPlayer = $null
        Write-Host "External preservation: engine PID $externalPid survived player quit"

        & $StopScript -Port $script:Port -TimeoutSeconds $QuitTimeoutSeconds
        if (-not $?) {
            throw 'The existing external stop script failed.'
        }
        Wait-ProcessGone $externalIdentity 'External stop'
        Wait-ListenerGone 'External stop'
        $script:ExternalLifecycle = $false
        Write-Host 'External preservation: exact ownership stop completed'
    }
}
catch {
    $primaryError = $_
    throw
}
finally {
    try {
        if ($null -ne $script:CurrentPlayer -and
            (Test-ProcessIdentity $script:CurrentPlayer.Process $DevExe $script:CurrentPlayer.StartedUtc)) {
            Close-DirectPlayer $script:CurrentPlayer -AllowForce
            $script:CurrentPlayer = $null
        }
        if ($script:ExternalLifecycle) {
            & $StopScript -Port $script:Port -TimeoutSeconds $QuitTimeoutSeconds
            if (-not $?) {
                throw 'Cleanup could not stop the script-owned external engine.'
            }
            $script:ExternalLifecycle = $false
        }
        if ($script:MayChangeLiveState) {
            $deadline = [DateTime]::UtcNow.AddSeconds($QuitTimeoutSeconds)
            while (@(Get-ExactEngines).Count -ne 0 -and [DateTime]::UtcNow -lt $deadline) {
                Start-Sleep -Milliseconds 150
            }
            if (@(Get-ExactEngines).Count -ne 0) {
                throw 'Cleanup refused to restore dev while an exact engine remains running.'
            }
            Wait-ListenerGone 'Cleanup'

            $restorePlayer = Start-DevPlayer 'Final restore'
            $restored = Wait-ManagedSession $restorePlayer 'Final restore'
            $script:CurrentPlayer = $null
            Write-Host "Dev instance restored and running: player PID $($restored.Player.Process.Id), engine PID $($restored.Engine.Process.Id)"
        }
    }
    catch {
        if ($null -eq $primaryError) {
            throw
        }
        Write-Warning "Cleanup/restore also failed: $($_.Exception.Message)"
    }
    finally {
        $script:BridgeToken = $null
    }
}
