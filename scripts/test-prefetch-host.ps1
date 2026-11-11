#Requires -Version 7.2
<#
.SYNOPSIS
    Drive the in-process PrefetchHost callback/generation regression.

.DESCRIPTION
    Writes a one-shot marker under the repository's .local/fb2k-dev profile and waits for
    cache/prefetch_probe.cpp to return sanitized JSON. This script never starts, stops or
    controls foobar2000, playback, the playlist or the output device.

.EXAMPLE
    pwsh -File scripts/test-prefetch-host.ps1
#>
[CmdletBinding()]
param(
    [ValidateRange(5, 120)]
    [int]$TimeoutSeconds = 30
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Instance = Join-Path $Root '.local/fb2k-dev'
$Exe = Join-Path $Instance 'foobar2000.exe'
$ProbeDir = Join-Path $Instance 'profile/glasswave/prefetch-probe'
$RequestPath = Join-Path $ProbeDir 'request.json'
$ClaimedPath = Join-Path $ProbeDir 'request.claimed.json'
$ResultPath = Join-Path $ProbeDir 'result.json'
$EvidenceDir = Join-Path $Root '.local/tests'
$EvidencePath = Join-Path $EvidenceDir 'prefetch-host-result.json'

if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) {
    throw 'Dev instance missing; this script does not bootstrap it.'
}

$devProcesses = @(
    Get-Process -Name foobar2000 -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -eq (Resolve-Path -LiteralPath $Exe).Path }
)
if ($devProcesses.Count -gt 0) {
    throw 'The dev instance is running. Close it before preparing this one-shot startup marker.'
}

New-Item -ItemType Directory -Force -Path $ProbeDir, $EvidenceDir | Out-Null
Remove-Item -LiteralPath $RequestPath, $ClaimedPath, $ResultPath, $EvidencePath `
    -Force -ErrorAction SilentlyContinue

$runId = [Guid]::NewGuid().ToString('N')
$request = [ordered]@{
    probe = 'prefetch-host'
    runId = $runId
}
[IO.File]::WriteAllText(
    $RequestPath,
    ($request | ConvertTo-Json -Compress),
    [Text.UTF8Encoding]::new($false))
Write-Host "PrefetchHost marker prepared; start the dev instance externally. Run-ID: $runId"

$result = $null
try {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (Test-Path -LiteralPath $ResultPath -PathType Leaf) {
            try {
                $result = Get-Content -LiteralPath $ResultPath -Raw |
                    ConvertFrom-Json -ErrorAction Stop
                break
            } catch {
                Start-Sleep -Milliseconds 100
            }
        }
        Start-Sleep -Milliseconds 100
    }
    if ($null -eq $result) {
        throw "PrefetchHost probe did not finish within $TimeoutSeconds seconds."
    }

    Copy-Item -LiteralPath $ResultPath -Destination $EvidencePath -Force
    $raw = Get-Content -LiteralPath $EvidencePath -Raw
    if ($raw -match '(?i)https?://|bearer\s|api[_-]?key|session(?:id|token)') {
        throw 'PrefetchHost evidence contains a URL or credential-shaped value.'
    }

    $required = @(
        'probe', 'provider', 'runId', 'ok', 'error', 'hookInstalled', 'hookRestored',
        'activeAfter', 'cases', 'caseCount', 'events', 'eventCount'
    )
    foreach ($name in $required) {
        if (-not $result.PSObject.Properties.Name.Contains($name)) {
            throw 'PrefetchHost result is incomplete.'
        }
    }
    if ($result.probe -cne 'prefetch-host' -or
        $result.provider -cne 'glasswave-prefetch-test' -or
        $result.runId -cne $runId) {
        throw 'PrefetchHost result identity mismatch.'
    }

    $names = @(
        'callback_host_installed',
        'threshold_before_30_seconds',
        'threshold_at_30_seconds',
        'progress_single_flight',
        'context_change_cancel',
        'context_change_replacement',
        'candidate_change_cancel',
        'candidate_change_replacement',
        'stop_after_current_cancel',
        'no_replacement_after_stop',
        'hooks_restored'
    )
    if (@($result.cases).Count -ne $names.Count) {
        throw 'PrefetchHost result must contain exactly eleven cases.'
    }
    for ($i = 0; $i -lt $names.Count; ++$i) {
        if ($result.cases[$i].name -cne $names[$i] -or -not $result.cases[$i].ok) {
            throw "PrefetchHost case failed: $($names[$i]) ($($result.cases[$i].detail))"
        }
    }
    if (-not $result.ok -or -not $result.hookInstalled -or -not $result.hookRestored -or
        [uint64]$result.activeAfter -ne 0 -or [int]$result.caseCount -ne $names.Count -or
        [int]$result.eventCount -ne 6) {
        throw 'PrefetchHost global assertions failed.'
    }

    $events = @($result.events)
    for ($i = 0; $i -lt $events.Count; ++$i) {
        $expected = if (($i % 2) -eq 0) { 'started' } else { 'cancelled' }
        if ($events[$i].kind -cne $expected) {
            throw 'PrefetchHost event sequence is not start/cancel alternating.'
        }
    }
    if ($events[0].itemPath -cne 'prefetch-probe-next-a' -or
        $events[0].proof -cne 'core-lookahead' -or
        [uint64]$events[2].contextGeneration -le [uint64]$events[0].contextGeneration -or
        $events[4].itemPath -cne 'prefetch-probe-next-b') {
        throw 'PrefetchHost generation or candidate assertions failed.'
    }

    $result.cases | Select-Object name, ok, detail | Format-Table -AutoSize
    Write-Host "PrefetchHost evidence: $EvidencePath"
} finally {
    if ($null -eq $result) {
        Remove-Item -LiteralPath $RequestPath, $ClaimedPath -Force -ErrorAction SilentlyContinue
    }
}
