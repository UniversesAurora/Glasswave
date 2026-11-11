#Requires -Version 7.2
<#
.SYNOPSIS
    Run the native ListSession functional regression in the repository dev instance.

.DESCRIPTION
    Writes a one-shot absolute-path marker under .local/fb2k-dev/profile, starts only that
    dev instance, waits for stream/list_session_probe.cpp to publish sanitized JSON, and
    validates every bounded functional case. The probe uses a non-registered fake provider,
    memory-only list policy, an in-process page host, and controlled callbacks. It does not
    open production UI or use playback, playlists, accounts, credentials or the network.

.PARAMETER Deploy
    Deploy the selected build through scripts/run-dev.ps1 before launching.
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',

    [ValidateRange(10, 60)]
    [int]$TimeoutSeconds = 60,

    [switch]$Deploy
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Instance = Join-Path $Root '.local/fb2k-dev'
$Exe = Join-Path $Instance 'foobar2000.exe'
$ExpectedProfile = [IO.Path]::GetFullPath((Join-Path $Instance 'profile'))
$ActualProfile = [IO.Path]::GetFullPath((Join-Path $Root '.local/fb2k-dev/profile'))
if (-not $ActualProfile.Equals($ExpectedProfile, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The marker profile is not the repository dev-instance profile.'
}

$MarkerDir = Join-Path $ActualProfile 'glasswave/list-session-probe'
$RequestPath = Join-Path $MarkerDir 'request.json'
$ClaimedPath = Join-Path $MarkerDir 'request.claimed.json'
$ResultPath = Join-Path $MarkerDir 'result.json'
$EvidenceDir = Join-Path $Root '.local/tests'
$EvidencePath = Join-Path $EvidenceDir 'list-session-result.json'

if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) {
    throw 'Dev instance missing; bootstrap is outside this probe.'
}
if (-not (Test-Path -LiteralPath $ActualProfile -PathType Container)) {
    throw 'Dev-instance profile is missing.'
}

function Stop-DevInstance {
    if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) { return }
    $resolvedExe = (Resolve-Path -LiteralPath $Exe).Path
    $processes = @(
        Get-Process -Name foobar2000 -ErrorAction SilentlyContinue |
            Where-Object { $_.Path -eq $resolvedExe }
    )
    foreach ($process in $processes) {
        [void]$process.CloseMainWindow()
        if (-not $process.WaitForExit(8000)) {
            $process.Kill()
            [void]$process.WaitForExit(3000)
        }
    }
}

$devProcesses = @(
    Get-Process -Name foobar2000 -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -eq (Resolve-Path -LiteralPath $Exe).Path }
)
if ($devProcesses.Count -gt 0 -and -not $Deploy) {
    throw 'The dev instance is already running; rerun with -Deploy so startup consumes the marker.'
}

New-Item -ItemType Directory -Force -Path $MarkerDir, $EvidenceDir | Out-Null
Remove-Item -LiteralPath $RequestPath, $ClaimedPath, $ResultPath, $EvidencePath `
    -Force -ErrorAction SilentlyContinue

$runId = 'listsession-' + [Guid]::NewGuid().ToString('N')
$marker = [ordered]@{
    probe = 'list-session'
    runId = $runId
} | ConvertTo-Json -Compress
[IO.File]::WriteAllText($RequestPath, $marker, [Text.UTF8Encoding]::new($false))

$startedDevInstance = $false
$result = $null
try {
    if ($Deploy) {
        & (Join-Path $PSScriptRoot 'run-dev.ps1') -Configuration $Configuration -Kill
        $startedDevInstance = $true
    } else {
        Start-Process -FilePath $Exe -WorkingDirectory $Instance -WindowStyle Hidden | Out-Null
        $startedDevInstance = $true
    }

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
        throw "ListSession probe did not finish within $TimeoutSeconds seconds."
    }

    Copy-Item -LiteralPath $ResultPath -Destination $EvidencePath -Force
    $raw = Get-Content -LiteralPath $EvidencePath -Raw
    if ($raw -match '(?i)https?://|bearer\s|api[_-]?key|session(?:id|token)|[a-z]:\\\\') {
        throw 'ListSession evidence contains a URL, path or credential-shaped value.'
    }

    $required = @(
        'probe', 'provider', 'runId', 'ok', 'error', 'elapsedMs', 'cachePolicy',
        'diskCacheExercised', 'providerRequests', 'enrichmentRequests', 'cases', 'caseCount',
        'failureCodes', 'checks'
    )
    foreach ($name in $required) {
        if (-not $result.PSObject.Properties.Name.Contains($name)) {
            throw "ListSession result is missing $name."
        }
    }
    if ($result.probe -cne 'list-session' -or
        $result.provider -cne 'list-session-test' -or
        $result.runId -cne $runId -or
        $result.cachePolicy -cne 'Memory' -or
        [bool]$result.diskCacheExercised) {
        throw 'ListSession result identity or cache-policy assertion failed.'
    }

    $caseNames = @(
        'on_demand_deferred_single_page',
        'all_mode_page_progress',
        'duplicate_rows_retained',
        'two_leases_coalesce_isolated_cancel',
        'cache_context_identity',
        'cache_context_rejects_late_data',
        'last_lease_cancel_stops_auto_drain',
        'refresh_rejects_old_generation',
        'repeated_cursor_rejected',
        'empty_page_progress_bounded',
        'error_prefix_retry_same_cursor',
        'early_next_before_deferred_start',
        'cancelled_lease_demand_is_isolated',
        'metadata_total_cursor_atomicity',
        'remote_page_filter_sort',
        'remote_page_tiles_selection',
        'remote_page_destroy_cancels',
        'initial_publish_before_enrichment',
        'next_page_continues_while_enrichment_pending',
        'out_of_order_page_patches',
        'unchanged_metadata_advances_frontier',
        'declined_enrichment_releases_frontier',
        'refresh_rejects_old_metadata_patch',
        'cancel_rejects_old_metadata_patch',
        'invalid_patch_shape_and_identity_rejected',
        'duplicate_metadata_occurrences_preserved',
        'metadata_failure_does_not_hide_primary',
        'account_generation_rejects_old_metadata_patch',
        'restore_gate_and_soft_retirement',
        'remote_page_enrichment_presentation'
    )
    if ([int]$result.caseCount -ne $caseNames.Count -or @($result.cases).Count -ne $caseNames.Count) {
        throw 'ListSession result must contain exactly thirty cases.'
    }
    for ($index = 0; $index -lt $caseNames.Count; ++$index) {
        $case = $result.cases[$index]
        if ($case.name -cne $caseNames[$index] -or -not [bool]$case.ok) {
            throw "ListSession case failed: $($caseNames[$index]) ($($case.detail))"
        }
    }

    $checks = $result.checks
    $checkNames = @(
        'callbackDeferred', 'onDemandCallsBeforeNext', 'onDemandCounts', 'allProgress',
        'duplicateRows', 'duplicateIdOccurrences', 'coalescedCalls', 'isolatedCallbackDelta',
        'cacheContextRequestCount', 'sameContextCoalesced', 'changedContextIsolated',
        'capturedContextReused', 'lateContextFetchRejected', 'lateContextPrimaryRejected',
        'lateContextEnrichmentRejected', 'lateContextDataIsolated',
        'autoDrainCallsBeforeCancel', 'autoDrainCallsAfterLate', 'refreshedCount',
        'oldGenerationIgnored', 'refreshTokenSignalled', 'repeatedCursorCalls',
        'noProgressCalls', 'noProgressBounded', 'cancelTokenSignalled', 'errorPrefixCount',
        'retryCursorOffset', 'retrySameCursor', 'finalRetryCount', 'earlyNextRequests',
        'earlyNextCounts', 'cancelledNextRequestsBeforeB', 'cancelledNextRequestsAfterB',
        'cancelledNextCount', 'metadataRejectCount', 'metadataRetrySameCursor',
        'metadataRetryRepeatedCursorError', 'metadataRetryCount', 'metadataFinalComplete',
        'remoteInitialCount', 'remoteFilterCounts', 'remoteOnDemandCount', 'remoteSortedPages',
        'remoteTileRows', 'remoteTileGroupCount', 'remoteTilePresentationKept',
        'remoteDestroyCancelled', 'ordinaryMetadataReadyRows', 'ordinaryUpdatedRowsAbsent',
        'initialPrimaryBeforeEnrichment', 'initialMetadataReadyRows',
        'initialEnrichedVersionDelta', 'initialDataVersionDelta',
        'initialPatchTransportStable', 'initialUpdatedRange',
        'nextPageRequestsWhilePending', 'nextPageRowsBeforeEnrichment',
        'nextPageMetadataReadyBefore', 'nextPageTransportStable', 'outOfOrderReadyRows',
        'outOfOrderPatchesApplied', 'outOfOrderTransportStable',
        'outOfOrderConcurrencyBounded', 'metadataCancellationShared',
        'unchangedMetadataAdvanced', 'unchangedDataVersionStable',
        'unchangedUpdatedRowsAbsent', 'declinedEnrichmentReleased',
        'declinedEnrichmentNoCallback', 'declinedEnrichmentDataVersionStable',
        'refreshOldPatchRejected', 'refreshNewPatchApplied',
        'refreshPatchRequests', 'refreshOldCancellation', 'cancelPatchRejected',
        'cancelBaseRetained', 'invalidCountRejected', 'invalidIdentityRejected',
        'invalidMetadataReadyRows', 'invalidBaseRetained', 'duplicateMetadataOccurrences',
        'duplicateMetadataPatchedByOccurrence', 'failurePrimaryRetained',
        'failureMetadataReady', 'failureErrorAbsent', 'failureDataVersionStable',
        'failureUpdatedRowsAbsent', 'accountPatchRejected',
        'accountBaseRetained', 'restoreInitialDeferred', 'restoreInitialEmptyNoError',
        'restoreNextPreserved',
        'softRetirePrefixRetained', 'softRetireOldNetworkRejected',
        'softRetireNewNetworkApplied', 'remotePendingGated', 'remoteMetadataReveal',
        'remoteNextPendingGated', 'remoteSelectionFocusKept',
        'remoteSortedGroupedSearchUpdated', 'remoteMetadataRows',
        'remoteMetadataGroupCount', 'remoteMetadataFilterCount'
    )
    foreach ($name in $checkNames) {
        if (-not $checks.PSObject.Properties.Name.Contains($name)) {
            throw "ListSession checks are missing $name."
        }
    }

    if (-not [bool]$checks.callbackDeferred -or
        [uint64]$checks.onDemandCallsBeforeNext -ne 1 -or
        (@($checks.onDemandCounts) -join ',') -cne '100,150' -or
        (@($checks.allProgress) -join ',') -cne '100,200,250' -or
        [uint64]$checks.duplicateRows -ne 250 -or
        [uint64]$checks.duplicateIdOccurrences -ne 3 -or
        [uint64]$checks.coalescedCalls -ne 2 -or
        [uint64]$checks.isolatedCallbackDelta -ne 0) {
        throw 'ListSession page, duplicate or lease coalescing assertions failed.'
    }
    if ([uint64]$checks.cacheContextRequestCount -ne 2 -or
        -not [bool]$checks.sameContextCoalesced -or
        -not [bool]$checks.changedContextIsolated -or
        -not [bool]$checks.capturedContextReused -or
        -not [bool]$checks.lateContextFetchRejected -or
        -not [bool]$checks.lateContextPrimaryRejected -or
        -not [bool]$checks.lateContextEnrichmentRejected -or
        -not [bool]$checks.lateContextDataIsolated) {
        throw 'ListSession provider cache-context identity assertions failed.'
    }
    if ([uint64]$checks.autoDrainCallsBeforeCancel -ne 2 -or
        [uint64]$checks.autoDrainCallsAfterLate -ne 2 -or
        [uint64]$checks.refreshedCount -ne 2 -or
        -not [bool]$checks.oldGenerationIgnored -or
        -not [bool]$checks.refreshTokenSignalled -or
        -not [bool]$checks.cancelTokenSignalled) {
        throw 'ListSession cancellation, late-callback or generation assertions failed.'
    }
    if ([uint64]$checks.repeatedCursorCalls -ne 1 -or
        [uint64]$checks.noProgressCalls -ne 9 -or
        -not [bool]$checks.noProgressBounded) {
        throw 'ListSession repeated-cursor or empty-page bound assertions failed.'
    }
    if ([uint64]$checks.errorPrefixCount -ne 100 -or
        [uint64]$checks.retryCursorOffset -ne 100 -or
        -not [bool]$checks.retrySameCursor -or
        [uint64]$checks.finalRetryCount -ne 150) {
        throw 'ListSession error-prefix or retry-cursor assertions failed.'
    }
    if ([uint64]$checks.earlyNextRequests -ne 2 -or
        (@($checks.earlyNextCounts) -join ',') -cne '100,150' -or
        [uint64]$checks.cancelledNextRequestsBeforeB -ne 1 -or
        [uint64]$checks.cancelledNextRequestsAfterB -ne 2 -or
        [uint64]$checks.cancelledNextCount -ne 150) {
        throw 'ListSession deferred or per-lease demand assertions failed.'
    }
    if ([uint64]$checks.metadataRejectCount -ne 100 -or
        -not [bool]$checks.metadataRetrySameCursor -or
        [bool]$checks.metadataRetryRepeatedCursorError -or
        [uint64]$checks.metadataRetryCount -ne 200 -or
        -not [bool]$checks.metadataFinalComplete) {
        throw 'ListSession metadata/cursor atomicity assertions failed.'
    }
    if ([uint64]$checks.remoteInitialCount -ne 100 -or
        (@($checks.remoteFilterCounts) -join ',') -cne '10,25,35' -or
        [uint64]$checks.remoteOnDemandCount -ne 250 -or
        -not [bool]$checks.remoteSortedPages -or
        [uint64]$checks.remoteTileRows -ne 120 -or
        [uint64]$checks.remoteTileGroupCount -ne 30 -or
        -not [bool]$checks.remoteTilePresentationKept -or
        -not [bool]$checks.remoteDestroyCancelled) {
        throw 'ListSession remote page source assertions failed.'
    }
    if ([uint64]$checks.ordinaryMetadataReadyRows -ne 100 -or
        -not [bool]$checks.ordinaryUpdatedRowsAbsent -or
        -not [bool]$checks.initialPrimaryBeforeEnrichment -or
        [uint64]$checks.initialMetadataReadyRows -ne 0 -or
        [uint64]$checks.initialEnrichedVersionDelta -ne 1 -or
        [uint64]$checks.initialDataVersionDelta -ne 1 -or
        -not [bool]$checks.initialPatchTransportStable -or
        -not [bool]$checks.initialUpdatedRange) {
        throw 'ListSession initial metadata publication assertions failed.'
    }
    if ([uint64]$checks.nextPageRequestsWhilePending -ne 2 -or
        [uint64]$checks.nextPageRowsBeforeEnrichment -ne 4 -or
        [uint64]$checks.nextPageMetadataReadyBefore -ne 0 -or
        -not [bool]$checks.nextPageTransportStable -or
        (@($checks.outOfOrderReadyRows) -join ',') -cne '0,4,6' -or
        -not [bool]$checks.outOfOrderPatchesApplied -or
        -not [bool]$checks.outOfOrderTransportStable -or
        -not [bool]$checks.outOfOrderConcurrencyBounded -or
        -not [bool]$checks.metadataCancellationShared) {
        throw 'ListSession pending or out-of-order enrichment assertions failed.'
    }
    if (-not [bool]$checks.unchangedMetadataAdvanced -or
        -not [bool]$checks.unchangedDataVersionStable -or
        -not [bool]$checks.unchangedUpdatedRowsAbsent -or
        -not [bool]$checks.declinedEnrichmentReleased -or
        -not [bool]$checks.declinedEnrichmentNoCallback -or
        -not [bool]$checks.declinedEnrichmentDataVersionStable -or
        -not [bool]$checks.refreshOldPatchRejected -or
        -not [bool]$checks.refreshNewPatchApplied -or
        [uint64]$checks.refreshPatchRequests -ne 2 -or
        -not [bool]$checks.refreshOldCancellation) {
        throw 'ListSession unchanged metadata or refresh-generation assertions failed.'
    }
    if (-not [bool]$checks.cancelPatchRejected -or
        -not [bool]$checks.cancelBaseRetained -or
        -not [bool]$checks.invalidCountRejected -or
        -not [bool]$checks.invalidIdentityRejected -or
        [uint64]$checks.invalidMetadataReadyRows -ne 4 -or
        -not [bool]$checks.invalidBaseRetained) {
        throw 'ListSession cancellation or invalid metadata patch assertions failed.'
    }
    if ([uint64]$checks.duplicateMetadataOccurrences -ne 2 -or
        -not [bool]$checks.duplicateMetadataPatchedByOccurrence -or
        -not [bool]$checks.failurePrimaryRetained -or
        -not [bool]$checks.failureMetadataReady -or
        -not [bool]$checks.failureErrorAbsent -or
        -not [bool]$checks.failureDataVersionStable -or
        -not [bool]$checks.failureUpdatedRowsAbsent -or
        -not [bool]$checks.accountPatchRejected -or
        -not [bool]$checks.accountBaseRetained) {
        throw 'ListSession duplicate, failure or account-generation assertions failed.'
    }
    if (-not [bool]$checks.restoreInitialDeferred -or
        -not [bool]$checks.restoreInitialEmptyNoError -or
        -not [bool]$checks.restoreNextPreserved -or
        -not [bool]$checks.softRetirePrefixRetained -or
        -not [bool]$checks.softRetireOldNetworkRejected -or
        -not [bool]$checks.softRetireNewNetworkApplied) {
        throw 'ListSession restore gate or soft-retirement assertions failed.'
    }
    if (-not [bool]$checks.remotePendingGated -or
        -not [bool]$checks.remoteMetadataReveal -or
        -not [bool]$checks.remoteNextPendingGated -or
        -not [bool]$checks.remoteSelectionFocusKept -or
        -not [bool]$checks.remoteSortedGroupedSearchUpdated -or
        [uint64]$checks.remoteMetadataRows -ne 130 -or
        [uint64]$checks.remoteMetadataGroupCount -ne 2 -or
        [uint64]$checks.remoteMetadataFilterCount -ne 130) {
        throw 'ListSession remote metadata-ready presentation assertions failed.'
    }
    if ([uint64]$result.providerRequests -ne 73 -or
        [uint64]$result.enrichmentRequests -ne 20 -or
        @($result.failureCodes).Count -ne 0 -or
        -not [bool]$result.ok -or
        -not [string]::IsNullOrEmpty([string]$result.error)) {
        throw 'ListSession global assertions failed.'
    }

    $result.cases | Select-Object name, ok, detail | Format-Table -AutoSize
    Write-Host "ListSession evidence: $EvidencePath"
    $result.ok
} catch {
    Write-Host "ListSession artifacts: $EvidencePath"
    throw
} finally {
    if ($startedDevInstance) { Stop-DevInstance }
    Remove-Item -LiteralPath $RequestPath, $ClaimedPath, $ResultPath `
        -Force -ErrorAction SilentlyContinue
}
