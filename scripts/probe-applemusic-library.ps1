#Requires -Version 7.2
<#
.SYNOPSIS
    Run the one-shot Apple Music provider-library read probe.

.DESCRIPTION
    Writes a marker into the repository dev profile and optionally restarts the dev
    instance through scripts/run-dev.ps1. The component then records dynamic sections,
    playlist exclusion, favorites, songs, genres, composers, created-playlist details,
    background artwork decoding, no-catalog album artwork, cached-read timings and one artist
    journey without UI automation.
#>
[CmdletBinding()]
param(
    [ValidateRange(1, 180)]
    [int]$TimeoutSeconds = 180,

    [switch]$Deploy
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Instance = Join-Path $Root '.local/fb2k-dev'
$Exe = Join-Path $Instance 'foobar2000.exe'
$ProbeDir = Join-Path $Instance 'profile/glasswave/applemusic'
$RequestPath = Join-Path $ProbeDir 'browse-request.json'
$ResultPath = Join-Path $ProbeDir 'browse-result.json'
$BridgePath = Join-Path $ProbeDir 'bridge.json'

function Invoke-BridgeAppleRead {
    param(
        [Parameter(Mandatory)][string]$BridgeUri,
        [Parameter(Mandatory)][string]$ApiToken,
        [Parameter(Mandatory)][string]$Path
    )

    $payload = [ordered]@{
        method = 'GET'
        path = $Path
        body = $null
    } | ConvertTo-Json -Compress
    try {
        Invoke-RestMethod -Method Post -Uri "$BridgeUri/api/v1/glasswave/apple" `
            -Headers @{ 'X-API-Token' = $ApiToken; Accept = 'application/json' } `
            -ContentType 'application/json' -Body $payload -TimeoutSec 30 -ErrorAction Stop
    } catch {
        throw 'Authenticated Apple Music library read failed.'
    }
}

function Get-BridgeConfig {
    if (-not (Test-Path -LiteralPath $BridgePath -PathType Leaf)) {
        throw 'Apple Music bridge configuration is missing.'
    }
    try {
        $bridge = Get-Content -LiteralPath $BridgePath -Raw | ConvertFrom-Json -ErrorAction Stop
    } catch {
        throw 'Apple Music bridge configuration is invalid.'
    }
    $bridgeUri = ([string]$bridge.url).TrimEnd('/')
    $apiToken = [string]$bridge.token
    $bridgeMatch = [regex]::Match($bridgeUri, '^http://127\.0\.0\.1:(?<port>[0-9]{1,5})$')
    if (-not $bridgeMatch.Success -or [string]::IsNullOrWhiteSpace($apiToken)) {
        throw 'Apple Music bridge configuration is invalid.'
    }
    $bridgePort = [int]$bridgeMatch.Groups['port'].Value
    if ($bridgePort -lt 1 -or $bridgePort -gt 65535) {
        throw 'Apple Music bridge configuration is invalid.'
    }
    return [pscustomobject]@{
        Uri = $bridgeUri
        Token = $apiToken
    }
}

function Get-FavoritePlaylistId {
    $bridge = Get-BridgeConfig
    $initial = '/v1/me/library/playlists?limit=100&extend[library-playlists]=tags'
    $path = $initial
    $seen = @{}
    for ($page = 0; $page -lt 256; $page++) {
        if ([string]::IsNullOrWhiteSpace($path) -or $seen.ContainsKey($path)) {
            throw 'Apple Music playlist pagination is invalid.'
        }
        $seen[$path] = $true
        $response = Invoke-BridgeAppleRead -BridgeUri $bridge.Uri -ApiToken $bridge.Token -Path $path
        if (-not [string]::IsNullOrWhiteSpace([string]$response.error)) {
            throw 'Apple Music returned an error while reading library playlists.'
        }
        foreach ($item in @($response.data)) {
            $tags = @($item.attributes.tags)
            if ($tags -contains 'favorited') {
                $id = [string]$item.id
                if ([string]::IsNullOrWhiteSpace($id)) {
                    throw 'Apple Music returned a favorite playlist without an id.'
                }
                return $id
            }
        }

        $next = [string]$response.next
        if ([string]::IsNullOrWhiteSpace($next)) {
            throw 'The Favorite Songs system playlist was not found.'
        }
        if ($next.StartsWith('/')) {
            $path = $next
        } else {
            $nextUri = $null
            if (-not [Uri]::TryCreate($next, [UriKind]::Absolute, [ref]$nextUri) -or
                $nextUri.Scheme -ne 'https' -or
                $nextUri.Host -notin @('amp-api.music.apple.com', 'api.music.apple.com')) {
                throw 'Apple Music returned an invalid playlist pagination link.'
            }
            $path = $nextUri.PathAndQuery
        }
        if (($path -split '\?', 2)[0] -ne ($initial -split '\?', 2)[0]) {
            throw 'Apple Music returned an unexpected playlist pagination path.'
        }
    }
    throw 'Apple Music playlist pagination did not terminate.'
}

function Get-LibraryOnlyArtistId {
    $bridge = Get-BridgeConfig
    $initial = '/v1/me/library/artists?limit=100&include=catalog&platform=web'
    $path = $initial
    $seen = @{}
    for ($page = 0; $page -lt 256; $page++) {
        if ([string]::IsNullOrWhiteSpace($path) -or $seen.ContainsKey($path)) {
            throw 'Apple Music artist pagination is invalid.'
        }
        $seen[$path] = $true
        $response = Invoke-BridgeAppleRead -BridgeUri $bridge.Uri -ApiToken $bridge.Token -Path $path
        if (-not [string]::IsNullOrWhiteSpace([string]$response.error)) {
            throw 'Apple Music returned an error while reading library artists.'
        }
        foreach ($item in @($response.data)) {
            $id = [string]$item.id
            if ([string]::IsNullOrWhiteSpace($id)) { continue }
            if ($id.Length -gt 256 -or $id -notmatch '^r\.[A-Za-z0-9._-]+$') { continue }
            # Catalog relation presence is the discriminator; missing artwork is normal.
            $catalogIds = @(
                @($item.relationships.catalog.data) |
                    Where-Object {
                        $null -ne $_ -and
                        -not [string]::IsNullOrWhiteSpace([string]$_.id)
                    }
            )
            if ($catalogIds.Count -eq 0) {
                return $id
            }
        }

        $next = [string]$response.next
        if ([string]::IsNullOrWhiteSpace($next)) { return $null }
        if ($next.StartsWith('/')) {
            $path = $next
        } else {
            $nextUri = $null
            if (-not [Uri]::TryCreate($next, [UriKind]::Absolute, [ref]$nextUri) -or
                $nextUri.Scheme -ne 'https' -or
                $nextUri.Host -notin @('amp-api.music.apple.com', 'api.music.apple.com')) {
                throw 'Apple Music returned an invalid artist pagination link.'
            }
            $path = $nextUri.PathAndQuery
        }
        if (($path -split '\?', 2)[0] -ne ($initial -split '\?', 2)[0]) {
            throw 'Apple Music returned an unexpected artist pagination path.'
        }
    }
    throw 'Apple Music artist pagination did not terminate.'
}

if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) {
    throw 'Dev instance missing; bootstrap is out of scope for this probe.'
}

$devProcesses = @(
    Get-Process -Name foobar2000 -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -eq (Resolve-Path -LiteralPath $Exe).Path }
)
if ($devProcesses.Count -gt 0 -and -not $Deploy) {
    throw 'The dev instance is already running; rerun with -Deploy so the startup probe can consume the marker.'
}

$libraryOnlyArtistId = Get-LibraryOnlyArtistId
New-Item -ItemType Directory -Force -Path $ProbeDir | Out-Null
Remove-Item -LiteralPath $ResultPath -Force -ErrorAction SilentlyContinue
$request = [ordered]@{ probe = 'library' }
if (-not [string]::IsNullOrWhiteSpace($libraryOnlyArtistId)) {
    $request.libraryOnlyArtistId = $libraryOnlyArtistId
}
$requestJson = $request | ConvertTo-Json -Compress
if ([Text.Encoding]::UTF8.GetByteCount($requestJson) -gt 4096) {
    throw 'Library probe request marker exceeds the size limit.'
}
[IO.File]::WriteAllText(
    $RequestPath,
    $requestJson,
    [Text.UTF8Encoding]::new($false)
)

if ($Deploy) {
    & (Join-Path $Root 'scripts/run-dev.ps1') -Configuration Release
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
    throw "Library probe did not finish within $TimeoutSeconds seconds."
}

$required = @(
    'ok', 'error', 'failureCodes', 'sections', 'sectionCount', 'createdCount',
    'savedCount', 'favoriteCount', 'librarySongCount', 'genreCount', 'composerCount',
    'genreTrackCount', 'composerTrackCount', 'playlists', 'favoriteLeak',
    'cachedFetchPlaylistsMs', 'cachedFetchPlaylistsSnapshotMatch',
    'cachedFetchPlaylistsOrderMatch', 'cachedReadBudgetMs',
    'playlistDetails', 'playlistDetailCaseCount', 'artDecodes',
    'artDecodePipeline', 'playlistCoverDecodedCount', 'followedArtistCount', 'artistResolveAttemptCount',
    'artistResolved', 'artistAvatarPresent', 'artistAlbumCount',
    'artistAlbumFirstPageCount', 'artistAlbumFirstPageMore', 'artistAlbumCountMatch',
    'artistAlbumReadError', 'artistTopTrackCount',
    'artistTopTrackReadError', 'artistAvatarSampleAvailableCount',
    'artistAvatarSampleRequestedCount', 'artistAvatarSampleDecodedCount',
    'noCatalogLibrarySongCount', 'noCatalogSongSampleIds',
    'noCatalogAlbumSampleAvailableCount', 'noCatalogAlbumRequestedCount',
    'noCatalogAlbumDecodedCount',
    'libraryOnlyArtistRequested', 'libraryOnlyArtistSkipped',
    'libraryOnlyArtistResolved', 'libraryOnlyArtistAccepted',
    'libraryOnlyArtistAlbumCount', 'libraryOnlyArtistFirstPageCount',
    'libraryOnlyArtistFirstPageMore', 'libraryOnlyArtistPageComplete',
    'libraryOnlyArtistCountKnown', 'libraryOnlyArtistCountMatch',
    'libraryOnlyArtistFetchError', 'libraryOnlyArtistAlbumsReadError',
    'libraryOnlyArtistTrackCount', 'libraryOnlyArtistTrackMore',
    'libraryOnlyArtistTrackReadError', 'libraryOnlyArtistAlbumIdsPresent',
    'libraryOnlyArtistTracksUnranked', 'libraryOnlyArtistCreditsPresent',
    'libraryOnlyArtistTracksAccepted'
)
foreach ($name in $required) {
    if (-not $result.PSObject.Properties.Name.Contains($name)) {
        throw 'Library probe result is incomplete.'
    }
}

$failures = [Collections.Generic.List[string]]::new()
function Add-AcceptanceFailure {
    param([Parameter(Mandatory)][string]$Code)
    if (-not $failures.Contains($Code)) {
        $failures.Add($Code)
    }
}
foreach ($code in @($result.failureCodes)) {
    if (-not [string]::IsNullOrWhiteSpace([string]$code)) {
        Add-AcceptanceFailure ([string]$code)
    }
}
if (-not [bool]$result.ok -and $failures.Count -eq 0) {
    Add-AcceptanceFailure 'provider_probe_reported_failure'
}

$favoritePlaylistId = Get-FavoritePlaylistId
$resultIds = @(
    @($result.playlists) |
        ForEach-Object { [string]$_.id } |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
)
$favoriteLeak = $resultIds -contains $favoritePlaylistId
$result.favoriteLeak = $favoriteLeak
$result | Add-Member -NotePropertyName favoriteTagDetected -NotePropertyValue $true -Force
if ($favoriteLeak) { Add-AcceptanceFailure 'favorite_playlist_leaked' }

$sections = @($result.sections)
if ($sections.Count -ne 8) { Add-AcceptanceFailure 'section_count_not_8' }
if ([int]$result.favoriteCount -le 0) { Add-AcceptanceFailure 'favorites_empty' }
if ([int]$result.librarySongCount -le 0) { Add-AcceptanceFailure 'library_songs_empty' }
if ([int]$result.genreCount -le 0 -or [int]$result.genreTrackCount -le 0) {
    Add-AcceptanceFailure 'genre_category_empty'
}
if ([int]$result.composerCount -le 0 -or [int]$result.composerTrackCount -le 0) {
    Add-AcceptanceFailure 'composer_category_empty'
}

if ([int]$result.createdCount -ne 3) { Add-AcceptanceFailure 'created_playlist_count_not_3' }
$playlistDetails = @($result.playlistDetails)
if ($playlistDetails.Count -ne [int]$result.createdCount -or
    [int]$result.playlistDetailCaseCount -ne [int]$result.createdCount) {
    Add-AcceptanceFailure 'playlist_detail_case_count_mismatch'
}
$cachedBudgetMs = [double]$result.cachedReadBudgetMs
if (-not [double]::IsFinite([double]$result.cachedFetchPlaylistsMs) -or
    [double]$result.cachedFetchPlaylistsMs -ge $cachedBudgetMs) {
    Add-AcceptanceFailure 'fetch_playlists_cache_slow'
}
if (-not [bool]$result.cachedFetchPlaylistsSnapshotMatch) {
    Add-AcceptanceFailure 'fetch_playlists_cache_snapshot_mismatch'
}
if (-not [bool]$result.cachedFetchPlaylistsOrderMatch) {
    Add-AcceptanceFailure 'fetch_playlists_cache_order_mismatch'
}
foreach ($row in $playlistDetails) {
    $position = [int]$row.position
    if (-not [bool]$row.countMatch) {
        Add-AcceptanceFailure "playlist_list_count_mismatch_$position"
    }
    if (-not [bool]$row.detailCountMatch) {
        Add-AcceptanceFailure "playlist_detail_count_mismatch_$position"
    }
    if (-not [double]::IsFinite([double]$row.cachedMs) -or [double]$row.cachedMs -ge $cachedBudgetMs) {
        Add-AcceptanceFailure "playlist_cached_read_slow_$position"
    }
    if (-not [bool]$row.cachedSnapshotMatch) {
        Add-AcceptanceFailure "playlist_cached_snapshot_mismatch_$position"
    }
    if (-not [bool]$row.ok) {
        Add-AcceptanceFailure "playlist_detail_failed_$position"
    }
}

$artDecodes = @($result.artDecodes)
if ([string]$result.artDecodePipeline -ne 'album_art_manager_v2+wic-background') {
    Add-AcceptanceFailure 'art_decode_pipeline_mismatch'
}
$playlistArts = @($artDecodes | Where-Object { [string]$_.kind -eq 'playlist' })
$artistArts = @($artDecodes | Where-Object { [string]$_.kind -eq 'artist' -and [bool]$_.requested })
$noCatalogAlbumArts = @(
    $artDecodes | Where-Object { [string]$_.kind -eq 'noCatalogAlbum' -and [bool]$_.requested }
)
if ($playlistArts.Count -ne [int]$result.createdCount) {
    Add-AcceptanceFailure 'playlist_cover_case_count_mismatch'
}
if ([int]$result.playlistCoverDecodedCount -ne [int]$result.createdCount) {
    Add-AcceptanceFailure 'playlist_cover_decode_incomplete'
}
foreach ($row in $playlistArts) {
    if (-not [bool]$row.decoded -or [uint32]$row.width -eq 0 -or [uint32]$row.height -eq 0) {
        Add-AcceptanceFailure "playlist_cover_decode_failed_$([int]$row.position)"
    }
}
foreach ($row in $artistArts) {
    if (-not [bool]$row.decoded -or [uint32]$row.width -eq 0 -or [uint32]$row.height -eq 0) {
        Add-AcceptanceFailure "artist_avatar_decode_failed_$([int]$row.position)"
    }
}
if ([int]$result.artistAvatarSampleRequestedCount -ne $artistArts.Count -or
    [int]$result.artistAvatarSampleDecodedCount -ne $artistArts.Count) {
    Add-AcceptanceFailure 'artist_avatar_sample_incomplete'
}
$noCatalogSongCount = [int]$result.noCatalogLibrarySongCount
if ($noCatalogSongCount -eq 0) {
    if ([int]$result.noCatalogAlbumSampleAvailableCount -ne 0 -or
        [int]$result.noCatalogAlbumRequestedCount -ne 0 -or
        [int]$result.noCatalogAlbumDecodedCount -ne 0 -or
        $noCatalogAlbumArts.Count -ne 0) {
        Add-AcceptanceFailure 'no_catalog_album_skip_state_invalid'
    }
} else {
    if ([int]$result.noCatalogAlbumSampleAvailableCount -lt 2) {
        Add-AcceptanceFailure 'no_catalog_album_sample_lt_2'
    }
    if ($noCatalogAlbumArts.Count -ne 2) {
        Add-AcceptanceFailure 'no_catalog_album_case_count_not_2'
    }
    if ([int]$result.noCatalogAlbumRequestedCount -ne 2 -or
        [int]$result.noCatalogAlbumRequestedCount -ne $noCatalogAlbumArts.Count) {
        Add-AcceptanceFailure 'no_catalog_album_requested_count_not_2'
    }
    if ([int]$result.noCatalogAlbumDecodedCount -ne 2) {
        Add-AcceptanceFailure 'no_catalog_album_decoded_count_not_2'
    }
    foreach ($row in $noCatalogAlbumArts) {
        if (-not [bool]$row.decoded -or [uint32]$row.width -eq 0 -or [uint32]$row.height -eq 0) {
            Add-AcceptanceFailure "no_catalog_album_decode_failed_$([int]$row.position)"
        }
    }
}

if ([int]$result.followedArtistCount -le 0) {
    Add-AcceptanceFailure 'artist_list_empty'
}
if (-not [bool]$result.artistResolved) {
    Add-AcceptanceFailure 'catalog_resolvable_artist_missing'
}
if (-not [bool]$result.artistAvatarPresent) {
    Add-AcceptanceFailure 'artist_avatar_missing'
}
if ([int]$result.artistAlbumCount -le 0) {
    Add-AcceptanceFailure 'artist_album_count_zero'
}
if ([int]$result.artistAlbumFirstPageCount -le 0) {
    Add-AcceptanceFailure 'artist_albums_empty'
}
if (-not [bool]$result.artistAlbumFirstPageMore -and
    ($null -eq $result.artistAlbumCountMatch -or -not [bool]$result.artistAlbumCountMatch)) {
    Add-AcceptanceFailure 'artist_album_count_mismatch'
}
if (-not [string]::IsNullOrWhiteSpace([string]$result.artistAlbumReadError)) {
    Add-AcceptanceFailure 'artist_albums_read_failed'
}
if ([int]$result.artistTopTrackCount -le 0) {
    Add-AcceptanceFailure 'artist_top_tracks_empty'
}
if (-not [string]::IsNullOrWhiteSpace([string]$result.artistTopTrackReadError)) {
    Add-AcceptanceFailure 'artist_top_tracks_read_failed'
}
if ([bool]$result.artistAvatarPresent -and
    [int]$result.artistAvatarSampleRequestedCount -le 0) {
    Add-AcceptanceFailure 'artist_avatar_sample_missing'
}

if (-not [string]::IsNullOrWhiteSpace($libraryOnlyArtistId)) {
    if (-not [bool]$result.libraryOnlyArtistRequested -or [bool]$result.libraryOnlyArtistSkipped) {
        Add-AcceptanceFailure 'library_only_artist_marker_not_consumed'
    }
    if (-not [bool]$result.libraryOnlyArtistResolved) {
        Add-AcceptanceFailure 'library_only_artist_fetch_failed'
    }
    if (-not [string]::IsNullOrWhiteSpace([string]$result.libraryOnlyArtistFetchError)) {
        Add-AcceptanceFailure 'library_only_artist_fetch_failed'
    }
    if ([int]$result.libraryOnlyArtistFirstPageCount -le 0) {
        Add-AcceptanceFailure 'library_only_artist_albums_empty'
    }
    if (-not [string]::IsNullOrWhiteSpace([string]$result.libraryOnlyArtistAlbumsReadError)) {
        Add-AcceptanceFailure 'library_only_artist_albums_read_failed'
    }
    if (-not [bool]$result.libraryOnlyArtistPageComplete) {
        Add-AcceptanceFailure 'library_only_artist_page_incomplete'
    }
    if (-not [bool]$result.libraryOnlyArtistCountKnown) {
        Add-AcceptanceFailure 'library_only_artist_album_count_unknown'
    }
    if (-not [bool]$result.libraryOnlyArtistCountMatch) {
        Add-AcceptanceFailure 'library_only_artist_album_count_mismatch'
    }
    if (-not [bool]$result.libraryOnlyArtistAccepted) {
        Add-AcceptanceFailure 'library_only_artist_not_accepted'
    }
    if ([int]$result.libraryOnlyArtistTrackCount -le 0) {
        Add-AcceptanceFailure 'library_only_artist_tracks_empty'
    }
    if (-not [string]::IsNullOrWhiteSpace([string]$result.libraryOnlyArtistTrackReadError)) {
        Add-AcceptanceFailure 'library_only_artist_tracks_read_failed'
    }
    if (-not [bool]$result.libraryOnlyArtistAlbumIdsPresent) {
        Add-AcceptanceFailure 'library_only_artist_album_ids_missing'
    }
    if (-not [bool]$result.libraryOnlyArtistTracksUnranked) {
        Add-AcceptanceFailure 'library_only_artist_tracks_fake_ranked'
    }
    if (-not [bool]$result.libraryOnlyArtistCreditsPresent) {
        Add-AcceptanceFailure 'library_only_artist_unrelated_track'
    }
    if (-not [bool]$result.libraryOnlyArtistTracksAccepted) {
        Add-AcceptanceFailure 'library_only_artist_tracks_not_accepted'
    }
} else {
    if ([bool]$result.libraryOnlyArtistRequested -or
        -not [bool]$result.libraryOnlyArtistSkipped -or
        -not [bool]$result.libraryOnlyArtistAccepted) {
        Add-AcceptanceFailure 'library_only_artist_skip_state_invalid'
    }
}

$result.failureCodes = @($failures)
$result.ok = $failures.Count -eq 0
$result.error = if ($failures.Count -eq 0) { '' } else { $failures[0] }
$result | Add-Member -NotePropertyName acceptanceFailureCount -NotePropertyValue $failures.Count -Force
$verifiedJson = $result | ConvertTo-Json -Depth 20 -Compress
[IO.File]::WriteAllText($ResultPath, $verifiedJson, [Text.UTF8Encoding]::new($false))

Write-Host "Sections: $($sections.Count)"
Write-Host "Created playlists: $($result.createdCount)"
Write-Host "Saved playlists: $($result.savedCount)"
Write-Host "Favorites: $($result.favoriteCount)"
Write-Host "Library songs: $($result.librarySongCount)"
if ($result.PSObject.Properties.Name -contains 'pagedFirstMs') {
    Write-Host ("Paged playlist: first {0} rows in {1:F1} ms; {2} rows/{3} pages in {4:F1} ms" -f
        $result.pagedFirstRows, [double]$result.pagedFirstMs, $result.pagedFinalRows,
        $result.pagedPages, [double]$result.pagedFinalMs)
    if ($result.PSObject.Properties.Name -contains 'pagedMetadataFinalMs') {
        Write-Host ("Related metadata: {0} first-page rows ready; all available metadata in {1:F1} ms" -f
            $result.pagedFirstMetadataReadyRows, [double]$result.pagedMetadataFinalMs)
    }
}
Write-Host "Genres: $($result.genreCount) ($($result.genreTrackCount) sampled tracks)"
Write-Host "Composers: $($result.composerCount) ($($result.composerTrackCount) sampled tracks)"
foreach ($row in $playlistDetails) {
    Write-Host ("Playlist {0}: list={1}, detail={2}, cached={3:F1} ms" -f
        $row.position, $row.listCount, $row.detailCount, [double]$row.cachedMs)
}
Write-Host ("Cached FetchPlaylists: {0:F1} ms" -f [double]$result.cachedFetchPlaylistsMs)
Write-Host "Playlist covers decoded: $($result.playlistCoverDecodedCount)/$($result.createdCount)"
Write-Host "Artist: albums=$($result.artistAlbumFirstPageCount), declared=$($result.artistAlbumCount), hot songs=$($result.artistTopTrackCount)"
Write-Host "Artist avatars decoded: $($result.artistAvatarSampleDecodedCount)/$($result.artistAvatarSampleRequestedCount)"
if ($noCatalogSongCount -eq 0) {
    Write-Host 'No-catalog album covers: skipped (no no-catalog songs)'
} else {
    Write-Host (
        "No-catalog album covers decoded: " +
        "$($result.noCatalogAlbumDecodedCount)/$($result.noCatalogAlbumRequestedCount)"
    )
}
if ([bool]$result.libraryOnlyArtistSkipped) {
    Write-Host 'Library-only artist fallback: skipped (no unmapped candidate)'
} else {
    Write-Host "Library-only artist fallback: albums=$($result.libraryOnlyArtistFirstPageCount), tracks=$($result.libraryOnlyArtistTrackCount), declared=$($result.libraryOnlyArtistAlbumCount)"
}
Write-Host "Favorite playlist leak: $favoriteLeak"
if ($failures.Count -gt 0) {
    Write-Host "Acceptance failures: $($failures -join ', ')"
}
if (-not $result.ok) {
    $message = if ([string]::IsNullOrWhiteSpace([string]$result.error)) {
        'unspecified failure'
    } else {
        [string]$result.error
    }
    Write-Host "Error: $message"
    exit 1
}
