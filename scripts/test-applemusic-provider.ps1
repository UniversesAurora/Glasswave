#Requires -Version 7.2
<#
.SYNOPSIS
    Exercise Apple Music provider writes and paged playlist reads against the isolated mock.

.DESCRIPTION
    This checks the original 19-operation write contract plus streaming FetchListPage
    paging, nonblocking page enrichment, duplicate-row
    preservation, and regional display
    metadata without changing the source US library or playback requests.
    It never contacts Apple or writes a user library.
    The real dev bridge is backed up, replaced by a random-port local mock, restored
    byte-for-byte and ACL-for-ACL in finally, and only the repository dev instance is used.

.PARAMETER Deploy
    Deploy the current Release build through scripts/run-dev.ps1 before launching. Omit this
    when the coordinated test sequence already deployed with run-dev.ps1 -NoLaunch.
#>
[CmdletBinding()]
param(
    [ValidateRange(1, 120)]
    [int]$TimeoutSeconds = 60,

    [ValidateSet('baseline', 'mutation-reconciliation', 'catalog-regression', 'catalog-suggestions')]
    [string]$Scenario = 'baseline',

    [switch]$Deploy
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Instance = Join-Path $Root '.local/fb2k-dev'
$Exe = Join-Path $Instance 'foobar2000.exe'
$ProbeDir = Join-Path $Instance 'profile/glasswave/applemusic'
$BridgePath = Join-Path $ProbeDir 'bridge.json'
$RequestPath = Join-Path $ProbeDir 'provider-test-request.json'
$ResultPath = Join-Path $ProbeDir 'provider-test-result.json'
$TestRoot = Join-Path $Root '.local/tests'
$StatePath = Join-Path $TestRoot "applemusic-provider-mock-$PID.state.json"
$MockBridgePath = Join-Path $TestRoot "applemusic-provider-mock-$PID.bridge.json"
$MockErrorPath = Join-Path $TestRoot 'applemusic-provider-mock-error.log'
$PerformanceEvidencePath = Join-Path $TestRoot 'applemusic-provider-performance.json'

if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) {
    throw 'Dev instance missing; bootstrap is out of scope for this test.'
}

function Stop-DevInstance {
    $exePath = (Resolve-Path -LiteralPath $Exe).Path
    $processes = @(
        Get-Process -Name foobar2000 -ErrorAction SilentlyContinue |
            Where-Object {
                try { $_.Path -eq $exePath }
                catch { $false }
            }
    )
    foreach ($process in $processes) {
        [void]$process.CloseMainWindow()
        if (-not $process.WaitForExit(5000)) {
            $process.Kill()
            [void]$process.WaitForExit(3000)
        }
    }
}

function Read-MockState {
    param(
        [Parameter(Mandatory)][string]$BridgeFile,
        [Parameter(Mandatory)][string]$Nonce
    )
    try {
        $bridge = Get-Content -LiteralPath $BridgeFile -Raw | ConvertFrom-Json -ErrorAction Stop
        $uri = [Uri]$bridge.url
        if ($uri.Scheme -ne 'http' -or $uri.Host -ne '127.0.0.1' -or $uri.Port -eq 20025 -or
            -not [string]::IsNullOrEmpty($uri.AbsolutePath.Trim('/')) -or
            [string]::IsNullOrWhiteSpace([string]$bridge.token)) {
            throw 'invalid mock bridge'
        }
        $testing = Invoke-RestMethod -Method Get -Uri "$($bridge.url)/api/v1/glasswave/testing" `
            -Headers @{ 'X-API-Token' = [string]$bridge.token } -TimeoutSec 10 -ErrorAction Stop
    } catch {
        throw 'Mock bridge verification failed.'
    }
    if (-not $testing.ok -or [string]$testing.nonce -ne $Nonce) {
        throw 'Mock bridge nonce verification failed.'
    }
    return $testing
}

$savedBridge = $false
$savedBridgeBytes = $null
$savedBridgeAcl = $null
$bridgeReplaced = $false
$mockProcess = $null
$testFailure = $null
$nonce = [Convert]::ToHexString(
    [Security.Cryptography.RandomNumberGenerator]::GetBytes(32)
).ToLowerInvariant()
$enrichmentTag = $nonce.Substring(0, 16)

try {
    New-Item -ItemType Directory -Force -Path $TestRoot | Out-Null
    Remove-Item -LiteralPath $StatePath, $MockBridgePath -Force -ErrorAction SilentlyContinue
    $node = (& mise which node).Trim()
    if ($LASTEXITCODE -ne 0 -or -not $node) { throw 'The project Node runtime is unavailable.' }
    $mockScript = Join-Path $Root 'tests/applemusic_provider_mock.mjs'
    $mockArguments = @(
        ('"' + $mockScript + '"'),
        '--state-file', ('"' + $StatePath + '"'),
        '--bridge-file', ('"' + $MockBridgePath + '"'),
        '--nonce', $nonce,
        '--scenario', $Scenario
    )
    $mockProcess = Start-Process -FilePath $node -ArgumentList $mockArguments -WorkingDirectory $Root `
        -WindowStyle Hidden -PassThru -RedirectStandardError $MockErrorPath

    $readyDeadline = [DateTime]::UtcNow.AddSeconds(10)
    while (-not (Test-Path -LiteralPath $StatePath -PathType Leaf) -or
           -not (Test-Path -LiteralPath $MockBridgePath -PathType Leaf)) {
        if ($mockProcess.HasExited -or [DateTime]::UtcNow -ge $readyDeadline) {
            throw 'The provider mock did not start.'
        }
        Start-Sleep -Milliseconds 100
    }
    $initialMock = Get-Content -LiteralPath $StatePath -Raw | ConvertFrom-Json
    if (-not $initialMock.ready) { throw 'The provider mock is not ready.' }

    Stop-DevInstance
    $savedBridge = Test-Path -LiteralPath $BridgePath -PathType Leaf
    if ($savedBridge) {
        $savedBridgeBytes = [IO.File]::ReadAllBytes($BridgePath)
        $savedBridgeAcl = [IO.FileSystemAclExtensions]::GetAccessControl(
            [IO.FileInfo]::new($BridgePath), [Security.AccessControl.AccessControlSections]::Access)
    }
    New-Item -ItemType Directory -Force -Path $ProbeDir | Out-Null
    $bridgeReplaced = $true
    Copy-Item -LiteralPath $MockBridgePath -Destination $BridgePath -Force
    Remove-Item -LiteralPath $ResultPath -Force -ErrorAction SilentlyContinue
    [IO.File]::WriteAllText(
        $RequestPath,
        (@{ nonce = $nonce; scenario = $Scenario } | ConvertTo-Json -Compress),
        [Text.UTF8Encoding]::new($false)
    )

    if ($Deploy) {
        & (Join-Path $Root 'scripts/run-dev.ps1') -Configuration Release -Kill
        Start-Process -FilePath $Exe -WorkingDirectory $Instance -ArgumentList '/stop' -WindowStyle Hidden |
            Out-Null
    } else {
        Start-Process -FilePath $Exe -WorkingDirectory $Instance -ArgumentList '/stop' -WindowStyle Hidden |
            Out-Null
    }

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $result = $null
    while ([DateTime]::UtcNow -lt $deadline) {
        if (Test-Path -LiteralPath $ResultPath -PathType Leaf) {
            try {
                $result = Get-Content -LiteralPath $ResultPath -Raw | ConvertFrom-Json -ErrorAction Stop
                break
            } catch {
                Start-Sleep -Milliseconds 100
            }
        }
        Start-Sleep -Milliseconds 200
    }
    if ($null -eq $result) {
        throw "Provider contract test did not finish within $TimeoutSeconds seconds."
    }

    $testing = Read-MockState -BridgeFile $MockBridgePath -Nonce $nonce
    [IO.File]::WriteAllText((Join-Path $TestRoot 'applemusic-provider-contract-state.json'),
        ($testing | ConvertTo-Json -Depth 20), [Text.UTF8Encoding]::new($false))
    if ([string]::IsNullOrWhiteSpace([string]$testing.account.accountId) -or
        [string]$testing.account.name -ne 'Provider Contract Mock' -or
        [string]$testing.account.storefront -ne 'US') {
        throw 'The mock account contract is missing a stable id or display name/country.'
    }
    if ($Scenario -eq 'catalog-suggestions') {
        $suggestionOperations = @(
            'catalog_suggestions_ordered',
            'catalog_suggestions_cancel',
            'catalog_suggestions_region_stale',
            'catalog_suggestions_account_stale'
        )
        $operations = @($result.operations)
        if (-not $result.ok) {
            throw "Suggestion provider probe reported failure: $($result.error)."
        }
        foreach ($name in $suggestionOperations) {
            $operation = $operations | Where-Object name -eq $name | Select-Object -First 1
            if ($null -eq $operation -or -not $operation.ok) {
                $detail = if ($null -eq $operation) { 'operation missing' } else { $operation.error }
                throw "Suggestion provider operation failed: $name ($detail)."
            }
        }
        if ($operations.Count -ne $suggestionOperations.Count) {
            throw "Suggestion operations: expected $($suggestionOperations.Count), received $($operations.Count)."
        }
        if (@($testing.errors).Count -ne 0 -or @($testing.unexpectedCalls).Count -ne 0) {
            throw ('Suggestion mock recorded errors or unexpected calls: errors={0}, unexpected={1}.' -f
                (@($testing.errors) -join '; '), (@($testing.unexpectedCalls) -join '; '))
        }
        if ([int]$testing.catalog.suggestionRequests -ne 3 -or
            [int]$testing.catalog.invalidSuggestionRequests -ne 0) {
            throw 'Suggestion HTTP/cancellation request counts failed.'
        }
        $zeroWrites = @($testing.writes.PSObject.Properties | Where-Object { [int]$_.Value -ne 0 })
        $zeroMutationWrites = @(
            $testing.mutation.writes.PSObject.Properties | Where-Object { [int]$_.Value -ne 0 }
        )
        if ($zeroWrites.Count -ne 0 -or $zeroMutationWrites.Count -ne 0) {
            throw 'Suggestion-only regression performed library writes.'
        }
        Write-Host "Suggestion operations: $($operations.Count)"
        Write-Host 'Suggestion order, cancellation, region stale and account stale: PASS'
        return
    }
    if ($Scenario -eq 'catalog-regression') {
        $catalogOperations = @(
            'catalog_artist_sections',
            'catalog_list_cache_opt_in',
            'catalog_search_kinds',
            'catalog_artist_section_pages',
            'catalog_artist_section_continuations',
            'catalog_artist_limit_forwarded',
            'catalog_artist_empty_404',
            'catalog_artist_unknown_404',
            'catalog_artist_401',
            'catalog_artist_network_error',
            'catalog_artist_continuation_404',
            'catalog_artist_concurrent_404_coalesced',
            'catalog_search_categories',
            'catalog_search_continuations',
            'catalog_search_mixed_terminal',
            'catalog_search_mixed_unknowns_continuation',
            'catalog_search_missing_category',
            'catalog_suggestions_ordered',
            'catalog_suggestions_cancel',
            'catalog_suggestions_region_stale',
            'catalog_suggestions_account_stale'
        )
        $operations = @($result.operations)
        if (-not $result.ok) {
            throw "Catalog provider probe reported failure: $($result.error)."
        }
        foreach ($name in $catalogOperations) {
            $operation = $operations | Where-Object name -eq $name | Select-Object -First 1
            if ($null -eq $operation -or -not $operation.ok) {
                $detail = if ($null -eq $operation) { 'operation missing' } else { $operation.error }
                throw "Catalog provider operation failed: $name ($detail)."
            }
        }
        if ($operations.Count -ne $catalogOperations.Count) {
            throw "Catalog provider operations: expected $($catalogOperations.Count), received $($operations.Count)."
        }
        if (@($testing.errors).Count -ne 0 -or @($testing.unexpectedCalls).Count -ne 0) {
            throw ('Catalog mock recorded errors or unexpected calls: errors={0}, unexpected={1}.' -f
                (@($testing.errors) -join '; '), (@($testing.unexpectedCalls) -join '; '))
        }
        $catalog = $testing.catalog
        if ($null -eq $catalog -or
            [int]$catalog.artistRoots.'1147783278' -lt 1 -or
            [int]$catalog.artistRoots.'1900003998' -lt 1 -or
            [int]$catalog.artistRoots.'1900003994' -ne 1 -or
            [int]$catalog.sectionRequests.'top-songs' -ne 3 -or
            [int]$catalog.sectionRequests.'featured-albums' -lt 3 -or
            [int]$catalog.sectionRequests.'full-albums' -lt 3 -or
            [int]$catalog.sectionRequests.'singles' -lt 3 -or
            [int]$catalog.sectionRequests.'live-albums' -lt 3 -or
            [int]$catalog.sectionRequests.'appears-on-albums' -ne 2 -or
            [int]$catalog.sectionRequests.'compilation-albums' -lt 3 -or
            [int]$catalog.sectionRequests.'featured-playlists' -lt 3 -or
            [int]$catalog.invalidSearchRequests -ne 0 -or
            [int]$catalog.invalidSearchContinuations -ne 0 -or
            [int]$catalog.invalidSuggestionRequests -ne 0 -or
            [int]$catalog.suggestionRequests -ne 3 -or
            @($catalog.sectionPaths | Select-Object -Unique).Count -lt 8) {
            throw 'Catalog mock request-shape or coalescing evidence failed.'
        }
        foreach ($kind in 'songs', 'artists', 'albums', 'playlists') {
            if ([int]$catalog.searchRequests.$kind -lt 2) {
                throw "Catalog search category $kind did not preserve an independent cursor."
            }
        }
        if ([int]$catalog.searchRequests.'songs,artists,albums,playlists' -lt 2) {
            throw 'Mixed top-results search did not issue the expected paged requests.'
        }
        $zeroWrites = @($testing.writes.PSObject.Properties | Where-Object { [int]$_.Value -ne 0 })
        $zeroMutationWrites = @(
            $testing.mutation.writes.PSObject.Properties | Where-Object { [int]$_.Value -ne 0 }
        )
        if ($zeroWrites.Count -ne 0 -or $zeroMutationWrites.Count -ne 0) {
            throw 'Catalog-only regression performed library writes.'
        }
        Write-Host "Catalog provider operations: $($operations.Count)"
        Write-Host 'Artist views: 8 independent descriptors, pages, continuations and coalesced 404 verification'
        Write-Host 'Catalog search: typed categories, mixed rank, unknown-type filtering and query rebinding'
        Write-Host 'Suggestions: interleaving preserved; cancellation and stale context drops verified'
        return
    }
    $expectedWrites = [ordered]@{
        favoriteAdd = 1
        favoriteRemove = 1
        playlistCreate = 1
        renameFailure = 1
        playlistRename = 1
        playlistAdd = 1
        playlistRemove = 1
        playlistDelete = 1
        albumSave = 1
        albumRemove = 1
        playlistSave = 1
        playlistRemoveFromLibrary = 1
    }
    foreach ($name in $expectedWrites.Keys) {
        $actual = [int]$testing.writes.$name
        if ($actual -ne $expectedWrites[$name]) {
            throw ("Unexpected mock write count for {0}: expected {1}, received {2}." -f
                $name, $expectedWrites[$name], $actual)
        }
    }
    if (@($testing.errors).Count -ne 0 -or @($testing.unexpectedCalls).Count -ne 0) {
        throw 'The mock recorded unexpected calls or contract errors.'
    }
    if ($null -eq $testing.isolation -or [int]$testing.isolation.invalidFavoriteTrackReads -ne 0) {
        throw 'The mock recorded an invalid favorite-track isolation request.'
    }
    if (-not $testing.rollbackRejected -or
        $testing.finalState.favorite -or
        $testing.finalState.albumSaved -or
        $testing.finalState.playlistSaved -or
        $testing.finalState.scratchExists) {
        throw 'Mock mutation invariants failed.'
    }

    $legacyExpectedOperations = @(
        'refresh', 'fetch_favorites_initial', 'fetch_library_songs',
        'favorite_add', 'favorite_remove', 'fetch_favorites_final',
        'playlist_create', 'rename_rollback', 'playlist_add',
        'playlist_rename',
        'playlist_fetch_added', 'playlist_remove', 'playlist_fetch_removed',
        'playlist_delete', 'album_save', 'album_unsave',
        'playlist_save', 'playlist_unsave', 'final_refresh'
    )
    $pagingExpectedOperations = @(
        'fetch_list_page_first_pause', 'fetch_list_page_next',
        'fetch_list_page_cursor_mismatch_rejected', 'fetch_list_page_cross_query_rejected',
        'fetch_list_page_exhaust', 'restore_list_records_merge'
    )
    $enrichmentExpectedOperations = @(
        'fetch_list_page_enrichment_baseline',
        'enrich_list_page_cancel_before_start',
        'enrich_list_page_relations',
        'enrich_list_page_fully_known_skipped'
    )
    $regionalExpectedOperations = @(
        'display_metadata_overlay',
        'display_metadata_region_guard',
        'provider_regional_cache_identity',
        'restore_list_page_preserves_canonical',
        'provider_display_region_japan',
        'provider_regional_japan_song_first',
        'provider_regional_japan_album',
        'provider_regional_japan_artist',
        'provider_regional_japan_retry',
        'provider_regional_japan_missing_artist',
        'provider_regional_japan_requests',
        'provider_display_region_china',
        'provider_regional_china_song_first',
        'provider_regional_china_album',
        'provider_regional_china_artist',
        'provider_regional_china_missing_artist',
        'provider_regional_china_requests',
        'provider_display_region_account',
        'provider_regional_account_album',
        'provider_regional_account_artist',
        'provider_regional_account_requests'
    )
    $mutationExpectedOperations = @(
        'album_remove_ack_reconcile',
        'album_catalog_resolver',
        'album_readd_catalog_identity',
        'playlist_save_ack_reconcile',
        'playlist_remove_ack_reconcile',
        'playlist_saved_authority_barrier',
        'rapid_album_remove_add',
        'account_switch_stale_epoch',
        'artist_favorite_add',
        'artist_favorite_remove',
        'artist_stale_read_ack_wins',
        'artist_rapid_last_intent'
    )
    $expectedOperations =
        $legacyExpectedOperations + $pagingExpectedOperations + $enrichmentExpectedOperations +
        $regionalExpectedOperations
    if ($Scenario -eq 'mutation-reconciliation') {
        $expectedOperations += $mutationExpectedOperations
    }
    $operations = @($result.operations)
    if (-not $result.ok) {
        throw "Provider probe reported failure: $($result.error)."
    }
    foreach ($name in $expectedOperations) {
        $operation = $operations | Where-Object name -eq $name | Select-Object -First 1
        if ($null -eq $operation -or -not $operation.ok) {
            $detail = if ($null -eq $operation) { 'operation missing' } else { $operation.error }
            throw "Provider operation failed: $name ($detail)."
        }
    }
    if ($legacyExpectedOperations.Count -ne 19 -or
        $pagingExpectedOperations.Count -ne 6 -or
        $enrichmentExpectedOperations.Count -ne 4 -or
        $regionalExpectedOperations.Count -ne 21 -or
        $mutationExpectedOperations.Count -ne 12 -or
        $operations.Count -ne $expectedOperations.Count) {
        throw ("Expected $($legacyExpectedOperations.Count) legacy, " +
            "$($pagingExpectedOperations.Count) paging, " +
            "$($enrichmentExpectedOperations.Count) enrichment, " +
            "$($regionalExpectedOperations.Count) regional display, and " +
            "$($mutationExpectedOperations.Count) mutation operations; " +
            "received $($operations.Count).")
    }

    $displayRegion = $result.displayRegion
    if ($null -eq $displayRegion -or
        $null -eq $displayRegion.saved -or
        [int]$displayRegion.test -ne 0 -or
        $null -eq $displayRegion.restored -or
        [int]$displayRegion.restored -ne [int]$displayRegion.saved -or
        -not [bool]$displayRegion.restoreAttempted -or
        -not [bool]$displayRegion.restoredOk -or
        -not [bool]$displayRegion.restoredByProvider) {
        throw 'The provider probe did not restore the owner metadata region exactly.'
    }

    $scenarioState = [string]$testing.scenario
    if ($scenarioState -ne $Scenario) {
        throw 'The mock scenario did not match the probe scenario.'
    }
    if ($Scenario -eq 'baseline') {
        if ([int]$testing.mutation.writes.albumRemove -ne 0 -or
            [int]$testing.mutation.writes.albumSave -ne 0 -or
            [int]$testing.mutation.writes.playlistSave -ne 0 -or
            [int]$testing.mutation.writes.playlistRemove -ne 0 -or
            [int]$testing.mutation.writes.artistFavoriteAdd -ne 0 -or
            [int]$testing.mutation.writes.artistFavoriteRemove -ne 0) {
            throw 'Baseline scenario executed mutation-ledger requests.'
        }
    } else {
        if ($scenarioState -ne 'mutation-reconciliation' -or
            [int]$testing.mutation.writes.albumRemove -ne 2 -or
            [int]$testing.mutation.writes.albumSave -ne 2 -or
            [int]$testing.mutation.writes.playlistSave -ne 1 -or
            [int]$testing.mutation.writes.playlistRemove -ne 1 -or
            [int]$testing.mutation.writes.artistFavoriteAdd -ne 3 -or
            [int]$testing.mutation.writes.artistFavoriteRemove -ne 3 -or
            -not [bool]$testing.finalState.mutationAlbumSaved -or
            -not [bool]$testing.finalState.mutationPlaylistSaved -or
            -not [bool]$testing.finalState.accountSwitched -or
            -not [bool]$result.mutationRegression.oldReadCompleted -or
            -not [bool]$result.accountSwitch.requested -or
            -not [bool]$result.accountSwitch.completed -or
            -not [bool]$result.accountSwitch.oldRefreshCompleted) {
            throw 'Mutation ledger scenario did not reach the expected final state.'
        }
        $authority = $testing.mutation.playlistAuthority
        if ($null -eq $authority -or
            -not [bool]$authority.staleServed -or
            -not [bool]$authority.absentServed -or
            -not [bool]$authority.externalPresent) {
            throw 'Playlist saved-state authority fixture did not reach all three snapshots.'
        }
        $artistFixture = $testing.mutation.artistFixture
        if ($null -eq $artistFixture -or
            [int]$artistFixture.originalLibraryReads -ne 0 -or
            [int]$artistFixture.canonicalReads.'1900002500' -lt 2 -or
            [int]$artistFixture.canonicalReads.'1900002501' -lt 1 -or
            -not [bool]$artistFixture.staleReadCompleted -or
            [bool]$testing.mutation.artistFavorites.'1542902428' -or
            [bool]$testing.mutation.artistFavorites.'1900002500' -or
            [bool]$testing.mutation.artistFavorites.'1900002501') {
            throw 'Artist favorite focused fixture validation failed.'
        }
        $writeResponses = @($testing.mutation.responses)
        $emptyFailures = @($writeResponses | Where-Object {
            [int]$_.status -in @(202, 204) -and [int]$_.bodyBytes -ne 0
        })
        $emptySuccesses = @($writeResponses | Where-Object {
            [int]$_.status -in @(202, 204) -and [int]$_.bodyBytes -eq 0
        })
        $nonemptyCreates = @($writeResponses | Where-Object {
            [int]$_.status -eq 201 -and [int]$_.bodyBytes -gt 0
        })
        $artistWrites = @($artistFixture.writes)
        $sourcePosts = @($artistWrites | Where-Object {
            [string]$_.id -eq '1900002500' -and [string]$_.method -eq 'POST'
        })
        $sourceDeletes = @($artistWrites | Where-Object {
            [string]$_.id -eq '1900002500' -and [string]$_.method -eq 'DELETE'
        })
        $stalePosts = @($artistWrites | Where-Object {
            [string]$_.id -eq '1900002501' -and [string]$_.method -eq 'POST'
        })
        $staleDeletes = @($artistWrites | Where-Object {
            [string]$_.id -eq '1900002501' -and [string]$_.method -eq 'DELETE'
        })
        if ($writeResponses.Count -ne 22 -or $emptyFailures.Count -ne 0 -or
            $emptySuccesses.Count -ne 20 -or $nonemptyCreates.Count -ne 2 -or
            $sourcePosts.Count -ne 2 -or $sourceDeletes.Count -ne 2 -or
            $stalePosts.Count -ne 1 -or $staleDeletes.Count -ne 1) {
            throw (("Mutation ACK audit: responses={0}/22, empty202204={1}/20, " +
                "nonempty201={2}/2, nonempty202204={3}/0, sourcePOST={4}/2, " +
                "sourceDELETE={5}/2, stalePOST={6}/1, staleDELETE={7}/1.") -f
                $writeResponses.Count, $emptySuccesses.Count, $nonemptyCreates.Count,
                $emptyFailures.Count, $sourcePosts.Count, $sourceDeletes.Count,
                $stalePosts.Count, $staleDeletes.Count)
        }
        $expectedAcks = @(
            @{ method='POST'; path='/v1/me/favorites'; resourceId='song.contract'; status=202; count=1 }
            @{ method='DELETE'; path='/v1/me/favorites'; resourceId='song.contract'; status=204; count=1 }
            @{ method='POST'; path='/v1/me/library/playlists'; resourceId='playlists'; status=201; count=1 }
            @{ method='POST'; path='/v1/me/library/playlists/p.scratch.contract/tracks'; resourceId='tracks'; status=201; count=1 }
            @{ method='DELETE'; path='/v1/me/library/playlists/p.scratch.contract/tracks'; resourceId='tracks'; status=204; count=1 }
            @{ method='DELETE'; path='/v1/me/library/playlists/p.scratch.contract'; resourceId='p.scratch.contract'; status=204; count=1 }
            @{ method='POST'; path='/v1/me/library'; resourceId='album.contract'; status=202; count=1 }
            @{ method='DELETE'; path='/v1/me/library/albums/l.album.contract'; resourceId='l.album.contract'; status=204; count=1 }
            @{ method='POST'; path='/v1/me/library'; resourceId='playlist.contract'; status=202; count=1 }
            @{ method='DELETE'; path='/v1/me/library/playlists/p.saved.contract'; resourceId='p.saved.contract'; status=204; count=1 }
            @{ method='POST'; path='/v1/me/library'; resourceId='1900000001'; status=202; count=2 }
            @{ method='DELETE'; path='/v1/me/library/albums/l.mutation.contract'; resourceId='l.mutation.contract'; status=204; count=2 }
            @{ method='POST'; path='/v1/me/library'; resourceId='1900000002'; status=202; count=1 }
            @{ method='DELETE'; path='/v1/me/library/playlists/p.mutation.contract'; resourceId='p.mutation.contract'; status=204; count=1 }
            @{ method='POST'; path='/v1/me/favorites'; resourceId='1900002500'; status=202; count=2 }
            @{ method='DELETE'; path='/v1/me/favorites'; resourceId='1900002500'; status=204; count=2 }
            @{ method='POST'; path='/v1/me/favorites'; resourceId='1900002501'; status=202; count=1 }
            @{ method='DELETE'; path='/v1/me/favorites'; resourceId='1900002501'; status=204; count=1 }
        )
        foreach ($ack in $expectedAcks) {
            $matching = @($writeResponses | Where-Object {
                [string]$_.method -eq $ack.method -and [string]$_.path -eq $ack.path -and
                [string]$_.resourceId -eq $ack.resourceId -and [int]$_.status -eq $ack.status
            })
            if ($matching.Count -ne $ack.count) {
                throw ("Mutation ACK {0} {1} id={2} HTTP {3}: expected {4}, received {5}." -f
                    $ack.method, $ack.path, $ack.resourceId, $ack.status, $ack.count, $matching.Count)
            }
        }
        Write-Host 'Mutation ACKs: 22 exact responses; 20 true empty 202/204; 2 nonempty 201'
    }

    $paging = $result.paging
    # Cursor rejection here verifies offset/opaque integrity; cursors remain reusable by design.
    if ($null -eq $paging -or
        [int]$paging.firstRows -ne 100 -or
        [int]$paging.secondRows -ne 100 -or
        [int]$paging.thirdRows -ne 100 -or
        [int]$paging.fourthRows -ne 59 -or
        [int]$paging.totalRows -ne 359 -or
        [int]$paging.uniqueTracks -ne 356 -or
        -not [bool]$paging.pausedAtFirstPage -or
        -not [bool]$paging.firstMore -or
        -not [bool]$paging.firstOffset -or
        -not [bool]$paging.firstOpaque -or
        -not [bool]$paging.explicitNextOffset -or
        -not [bool]$paging.mismatchedCursorRejected -or
        -not [bool]$paging.crossQueryRejected -or
        -not [bool]$paging.exhausted -or
        -not [bool]$paging.creditsValid -or
        -not [bool]$paging.restorePreserved) {
        throw 'Streaming list provider result validation failed.'
    }

    $paged = $testing.paged
    if ($null -eq $paged -or
        [int]$paged.primaryRequests -ne 1 -or
        [int]$paged.pageRequests -ne 3 -or
        [int]$paged.zeroOffsetRequests -ne 0 -or
        [int]$paged.exactNestedIncludes -ne 4 -or
        [int]$paged.opaqueQueryRetained -ne 3 -or
        [int]$paged.invalidPrimaryRequests -ne 0 -or
        [int]$paged.invalidPageRequests -ne 0 -or
        [int]$paged.pageRows -ne 359) {
        throw 'Streaming list provider mock request-shape validation failed.'
    }
    $pagedOffsets = @($paged.offsets | ForEach-Object { [int]$_ })
    if (($pagedOffsets -join ',') -ne '100,200,300') {
        throw 'Streaming list provider mock request-shape validation failed.'
    }

    $enrichment = $result.enrichment
    if ($null -eq $enrichment -or
        [string]$enrichment.tag -ne $enrichmentTag -or
        [int]$enrichment.baselineRows -ne 5 -or
        [int]$enrichment.enrichedRows -ne 5 -or
        [int]$enrichment.uniqueTracks -ne 4 -or
        [int]$enrichment.hydratedUniqueTracks -ne 3 -or
        [int]$enrichment.callbackCount -ne 1 -or
        [int]$enrichment.cancellationCallbacks -ne 0 -or
        [int]$enrichment.fullyKnownCallbacks -ne 0 -or
        [double]$enrichment.callbackMs -lt 100 -or
        -not [bool]$enrichment.freshCatalogIds -or
        -not [bool]$enrichment.baselineUsable -or
        -not [bool]$enrichment.baselinePending -or
        -not [bool]$enrichment.noHydrationBeforeBaseline -or
        -not [bool]$enrichment.cancellationRejected -or
        -not [bool]$enrichment.cancellationNoRequests -or
        -not [bool]$enrichment.explicitAccepted -or
        -not [bool]$enrichment.callbackNotInline -or
        -not [bool]$enrichment.occurrenceIdentityPreserved -or
        -not [bool]$enrichment.relationsValid -or
        -not [bool]$enrichment.pendingCleared -or
        -not [bool]$enrichment.knownRecordSkipped -or
        -not [bool]$enrichment.fullyKnownPageSkipped) {
        throw 'Nonblocking list enrichment result validation failed.'
    }

    $fixtureEnrichment = $testing.enrichment
    if ($null -eq $fixtureEnrichment) {
        throw 'Nonblocking list enrichment mock evidence is missing.'
    }
    $expectedHydrationIds = @(
        (2, 1, 3) | ForEach-Object { "i.song.glasswave-enrichment.$enrichmentTag.$_" }
    )
    $actualHydrationIds = @($fixtureEnrichment.hydrationIds)
    $enrichmentBatchSizes = @($fixtureEnrichment.hydrationBatchSizes | ForEach-Object { [int]$_ })
    if ([string]$fixtureEnrichment.tag -ne $enrichmentTag -or
        [int]$fixtureEnrichment.trackRequests -ne 1 -or
        [int]$fixtureEnrichment.invalidTrackRequests -ne 0 -or
        [int]$fixtureEnrichment.trackRows -ne 5 -or
        [int]$fixtureEnrichment.hydrationRequests -ne 1 -or
        [int]$fixtureEnrichment.hydrationRows -ne 3 -or
        [int]$fixtureEnrichment.invalidHydrationRequests -ne 0 -or
        [int]$fixtureEnrichment.knownIdRequests -ne 0 -or
        [int]$fixtureEnrichment.hydrationDuplicateIds -ne 0 -or
        [int]$fixtureEnrichment.maxHydrationConcurrency -ne 1 -or
        [int]$fixtureEnrichment.activeHydrationRequests -ne 0 -or
        [int]$fixtureEnrichment.hydrationDelayMs -lt 100 -or
        ($enrichmentBatchSizes -join ',') -ne '3' -or
        ($actualHydrationIds -join ',') -ne ($expectedHydrationIds -join ',')) {
        throw 'Nonblocking list enrichment mock request-shape validation failed.'
    }

    $regional = $result.regional
    if ($null -eq $regional -or
        -not [bool]$regional.directCacheContract -or
        -not [bool]$regional.staleWritesIsolated -or
        -not [bool]$regional.sourceStorefrontIsolated -or
        -not [bool]$regional.restoreCanonicalRecords -or
        -not [bool]$regional.japanProviderFetch -or
        -not [bool]$regional.chinaProviderFetch -or
        -not [bool]$regional.accountProviderFetch -or
        -not [bool]$regional.missingCounterpartFallback -or
        -not [bool]$regional.albumDescriptionOverlay -or
        -not [bool]$regional.artistBioOverlay -or
        -not [bool]$regional.identityDurationPreserved -or
        -not [bool]$regional.sourceUsRequestsOnly -or
        -not [bool]$regional.regionalListPersistent -or
        -not [bool]$regional.regionalCacheContextsSeparated -or
        -not [bool]$regional.transientRetryRecovered) {
        throw 'Regional display metadata provider validation failed.'
    }

    $fixtureRegional = $testing.regional
    if ($null -eq $fixtureRegional -or
        [int]$fixtureRegional.sourceLibraryAlbumRequests -ne 0 -or
        [int]$fixtureRegional.sourceLibraryAlbumTrackRequests -ne 0 -or
        [int]$fixtureRegional.sourceLibraryArtistRequests -ne 0 -or
        [int]$fixtureRegional.sourceCatalogArtistRequests -ne 2 -or
        [int]$fixtureRegional.sourceRegionalAlbumRequests -ne 5 -or
        [int]$fixtureRegional.sourceRegionalArtistRequests -ne 3 -or
        [int]$fixtureRegional.sourceRetryArtistRequests -ne 2 -or
        # Artist album counts are remembered across runs, so a repeated run may issue none of
        # these requests; whatever is issued must still have the source-storefront shape.
        [int]$fixtureRegional.invalidSourceArtistAlbumRequests -ne 0 -or
        [int]$fixtureRegional.japanAlbumRequests -ne 1 -or
        [int]$fixtureRegional.japanSongRequests -ne 1 -or
        [int]$fixtureRegional.japanArtistRequests -ne 4 -or
        [int]$fixtureRegional.chinaAlbumRequests -ne 1 -or
        [int]$fixtureRegional.chinaSongRequests -ne 1 -or
        [int]$fixtureRegional.chinaArtistRequests -ne 2 -or
        [int]$fixtureRegional.retryArtistRequests -ne 2 -or
        [int]$fixtureRegional.retryArtistFailures -ne 1 -or
        [int]$fixtureRegional.retryArtistSuccesses -ne 1 -or
        [int]$fixtureRegional.invalidRequests -ne 0) {
        throw 'Regional display metadata request-shape validation failed.'
    }
    if (-not $result.ok) { throw 'Provider contract test reported failure.' }

    Write-Host "Provider operations: $($operations.Count)"
    Write-Host "Mock writes: $(@($testing.writes.PSObject.Properties).Count)"
    Write-Host "Unexpected calls: $(@($testing.unexpectedCalls).Count)"
    Write-Host ("Owner metadata region: saved={0}, test={1}, restored={2}, restoredByProvider={3}" -f
        $displayRegion.saved, $displayRegion.test, $displayRegion.restored, $displayRegion.restoredByProvider)
    Write-Host 'Streaming list pages: 1 primary, 3 next pages, 359 rows, no offset=0 request'
    Write-Host 'Nonblocking enrichment: 5 baseline rows, 3 missed relations, 0 pre-hydration calls'
    Write-Host (("Regional display metadata: JP/CN direct details fetched after song-first " +
        "seeding; Account restored source US"))
    Write-Host 'Regional list cache: persistent contexts separated by region and source storefront'
    Write-Host 'Regional transient failure: one JP 503 retried without a region toggle'
    Write-Host 'Mock write and enrichment contracts: complete'
} catch {
    $testFailure = $_.Exception.Message
    throw
} finally {
    try { Stop-DevInstance } catch {}
    try {
        $mockEvidence = $null
        $probeEvidence = $null
        if (Test-Path -LiteralPath $StatePath -PathType Leaf) {
            $state = Get-Content -LiteralPath $StatePath -Raw | ConvertFrom-Json -ErrorAction Stop
            $mockEvidence = [ordered]@{
                writes = $state.writes
                errors = @($state.errors)
                unexpectedCalls = @($state.unexpectedCalls)
                rollbackRejected = $state.rollbackRejected
                performance = $state.performance
                paged = $state.paged
                enrichment = $state.enrichment
                regional = $state.regional
                catalog = $state.catalog
                startup = $state.startup
                isolation = $state.isolation
                scenario = $state.scenario
                mutation = $state.mutation
                accountControl = $state.accountControl
                finalState = $state.finalState
            }
        }
        if (Test-Path -LiteralPath $ResultPath -PathType Leaf) {
            $probeResult = Get-Content -LiteralPath $ResultPath -Raw | ConvertFrom-Json -ErrorAction Stop
            $probeEvidence = [ordered]@{
                ok = $probeResult.ok
                error = $probeResult.error
                operations = @($probeResult.operations)
                paging = $probeResult.paging
                enrichment = $probeResult.enrichment
                regional = $probeResult.regional
                startup = $probeResult.startup
                displayRegion = $probeResult.displayRegion
                accountSwitch = $probeResult.accountSwitch
                mutationRegression = $probeResult.mutationRegression
            }
        }
        [IO.File]::WriteAllText(
            $PerformanceEvidencePath,
            ([ordered]@{
                failed = -not [string]::IsNullOrWhiteSpace($testFailure)
                testFailure = $testFailure
                mock = $mockEvidence
                probe = $probeEvidence
            } | ConvertTo-Json -Depth 20),
            [Text.UTF8Encoding]::new($false)
        )
    } catch {}
    if ($bridgeReplaced) {
        if ($savedBridge) {
            New-Item -ItemType Directory -Force -Path $ProbeDir | Out-Null
            [IO.File]::WriteAllBytes($BridgePath, $savedBridgeBytes)
            if ($savedBridgeAcl) {
                [IO.FileSystemAclExtensions]::SetAccessControl([IO.FileInfo]::new($BridgePath), $savedBridgeAcl)
            }
            Write-Host 'Owner bridge: restored original bytes and access rules'
        } else {
            Remove-Item -LiteralPath $BridgePath -Force -ErrorAction SilentlyContinue
        }
    }
    if ($mockProcess -and -not $mockProcess.HasExited) {
        $mockProcess.Kill()
        [void]$mockProcess.WaitForExit(3000)
    }
    Remove-Item -LiteralPath $RequestPath, $StatePath, $MockBridgePath -Force -ErrorAction SilentlyContinue
}
