#Requires -Version 7.2
<#
.SYNOPSIS
    Drive the in-process PlaybackScheduler native queue/playlist regression.

.DESCRIPTION
    Prepares a one-shot marker under the repository dev profile. The caller must start the
    dev instance externally. The probe injects no audio: synthetic handles use real
    playlist_manager and SDK queue APIs, while StartOccurrence/CurrentQueueIndex are stubbed
    only after the marker is accepted.
#>
[CmdletBinding()]
param(
    [ValidateRange(5, 180)]
    [int]$TimeoutSeconds = 45
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Instance = Join-Path $Root '.local/fb2k-dev'
$Exe = Join-Path $Instance 'foobar2000.exe'
$ProbeDir = Join-Path $Instance 'profile/glasswave/scheduler-probe'
$RequestPath = Join-Path $ProbeDir 'request.json'
$ClaimedPath = Join-Path $ProbeDir 'request.claimed.json'
$ResultPath = Join-Path $ProbeDir 'result.json'
$EvidenceDir = Join-Path $Root '.local/tests'
$EvidencePath = Join-Path $EvidenceDir 'playback-scheduler-result.json'

if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) {
    throw 'Dev instance missing; this script does not bootstrap it.'
}
$DevProcesses = @(
    Get-Process -Name foobar2000 -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -eq (Resolve-Path -LiteralPath $Exe).Path }
)
if ($DevProcesses.Count -gt 0) {
    throw 'The dev instance is running. Close it before preparing this startup marker.'
}

New-Item -ItemType Directory -Force -Path $ProbeDir, $EvidenceDir | Out-Null
Remove-Item -LiteralPath $RequestPath, $ClaimedPath, $ResultPath, $EvidencePath `
    -Force -ErrorAction SilentlyContinue

$RunId = [Guid]::NewGuid().ToString('N')
$Request = [ordered]@{
    probe = 'playback-scheduler'
    runId = $RunId
}
[IO.File]::WriteAllText(
    $RequestPath,
    ($Request | ConvertTo-Json -Compress),
    [Text.UTF8Encoding]::new($false))
Write-Host "PlaybackScheduler marker prepared; start the dev instance externally. Run-ID: $RunId"

$Result = $null
try {
    $Deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $Deadline) {
        if (Test-Path -LiteralPath $ResultPath -PathType Leaf) {
            try {
                $Result = Get-Content -LiteralPath $ResultPath -Raw |
                    ConvertFrom-Json -ErrorAction Stop
                break
            } catch {
                Start-Sleep -Milliseconds 100
            }
        }
        Start-Sleep -Milliseconds 100
    }
    if ($null -eq $Result) {
        throw "PlaybackScheduler probe did not finish within $TimeoutSeconds seconds."
    }

    Copy-Item -LiteralPath $ResultPath -Destination $EvidencePath -Force
    if ((Get-Content -LiteralPath $EvidencePath -Raw) -match
        '(?i)https?://|bearer\s|api[_-]?key|session(?:id|token)') {
        throw 'PlaybackScheduler evidence contains a URL or credential-shaped value.'
    }
    $Required = @('probe', 'runId', 'ok', 'error', 'installed', 'restored', 'cases', 'caseCount')
    foreach ($Name in $Required) {
        if (-not $Result.PSObject.Properties.Name.Contains($Name)) {
            throw 'PlaybackScheduler result is incomplete.'
        }
    }
    if ($Result.probe -cne 'playback-scheduler' -or $Result.runId -cne $RunId) {
        throw 'PlaybackScheduler result identity mismatch.'
    }

    $Names = @(
        'empty_active_playing_playlist_restore',
        'explicit_start_keeps_duplicate_rows_in_place',
        'explicit_start_keeps_shuffled_rows_in_place',
        'play_next_prepends_foreign_queue',
        'owned_queue_mirror_follows_shuffle_reorder',
        'explicit_start_preserves_rows_and_play_next_queue',
        'explicit_start_consumes_only_selected_priority_row',
        'own_priority_drag_overrides_sdk_queue',
        'foreign_cross_boundary_drag_projects_prefix',
        'stale_snapshot_reorder_rejected',
        'add_delete_during_shuffle_preserve_content_generation',
        'replace_cancels_stale_append',
        'native_shuffle_imports_and_core_returns_default',
        'own_default_echo_does_not_disable_shuffle',
        'native_default_disables_custom_shuffle',
        'repeat_one_priority_is_actual_next',
        'repeat_cycle_peek_visible',
        'checkpoint_roles_and_host_maps_roundtrip'
    )
    if ([int]$Result.caseCount -ne $Names.Count -or @($Result.cases).Count -ne $Names.Count) {
        throw "PlaybackScheduler result must contain exactly $($Names.Count) cases."
    }
    for ($Index = 0; $Index -lt $Names.Count; ++$Index) {
        if ($Result.cases[$Index].name -cne $Names[$Index] -or -not $Result.cases[$Index].ok) {
            throw "PlaybackScheduler case failed: $($Names[$Index]) ($($Result.cases[$Index].detail))"
        }
    }
    if (-not $Result.ok -or -not $Result.installed -or -not $Result.restored -or
        $null -ne $Result.error) {
        throw "PlaybackScheduler global assertions failed: error=$($Result.error); " +
            "installed=$($Result.installed); restored=$($Result.restored)"
    }

    $Result.cases | Select-Object name, ok, detail | Format-Table -AutoSize
    Write-Host "PlaybackScheduler evidence: $EvidencePath"
} finally {
    if ($null -eq $Result) {
        Remove-Item -LiteralPath $RequestPath, $ClaimedPath -Force -ErrorAction SilentlyContinue
    }
}
