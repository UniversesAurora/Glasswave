#Requires -Version 7.2
<#
.SYNOPSIS
    Test authenticated encoded ALAC or AAC transport through the real foobar2000 input.
#>
[CmdletBinding()]
param(
    [string]$FixturePath = '',
    [ValidateSet('ALAC', 'AAC', 'ATMOS')]
    [string]$Codec = 'ALAC',
    [ValidateSet('StereoSpeakers', 'Headphones', 'Speakers51', 'Speakers71')]
    [string]$AtmosOutput = 'StereoSpeakers',
    [switch]$SlowStreamTest,
    [switch]$CancelStreamTest,
    [switch]$CacheReuseTest,
    [switch]$SeekStress,
    [switch]$MappingMatrixTest,
    [switch]$StaleCatalogTest,
    [ValidateSet('None', 'InvalidSessionMetadata', 'PreviewRejected', 'WrongLibrary', 'WrongReportingId')]
    [string]$StaleCatalogFailure = 'None'
)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
if ($CancelStreamTest -and -not $SlowStreamTest) {
    throw '-CancelStreamTest requires -SlowStreamTest.'
}
if ($CacheReuseTest -and $CancelStreamTest) {
    throw '-CacheReuseTest requires a full-song probe and cannot be combined with -CancelStreamTest.'
}
if ($StaleCatalogTest -and ($Codec -eq 'ATMOS' -or $SlowStreamTest -or $CacheReuseTest)) {
    throw '-StaleCatalogTest requires a normal ALAC or AAC probe.'
}
if (-not $StaleCatalogTest -and $StaleCatalogFailure -ne 'None') {
    throw '-StaleCatalogFailure requires -StaleCatalogTest.'
}
if ($MappingMatrixTest) {
    foreach ($name in $PSBoundParameters.Keys) {
        if ($name -notin @('MappingMatrixTest', 'SeekStress')) {
            throw "-MappingMatrixTest cannot be combined with -$name."
        }
    }
    $mappingCases = @(
        @{ Codec = 'ALAC'; Failure = 'None'; Label = 'ALAC catalog mapping' }
        @{ Codec = 'AAC'; Failure = 'None'; Label = 'AAC catalog mapping' }
        @{ Codec = 'ALAC'; Failure = 'PreviewRejected'; Label = 'preview rejection cleanup' }
        @{ Codec = 'ALAC'; Failure = 'InvalidSessionMetadata'; Label = 'invalid-session cleanup' }
        @{ Codec = 'ALAC'; Failure = 'WrongLibrary'; Label = 'wrong-library cleanup' }
        @{ Codec = 'ALAC'; Failure = 'WrongReportingId'; Label = 'wrong-reporting-id cleanup' }
    )
    foreach ($case in $mappingCases) {
        Write-Host "Mapping acceptance: $($case.Label)"
        $childArguments = @{
            Codec = $case.Codec
            StaleCatalogTest = $true
            StaleCatalogFailure = $case.Failure
        }
        if ($SeekStress -and $case.Failure -eq 'None') {
            $childArguments.SeekStress = $true
        }
        & $PSCommandPath @childArguments
    }
    return
}
$mappingMode = if (-not $StaleCatalogTest) {
    'Normal'
} elseif ($StaleCatalogFailure -eq 'None') {
    'StaleCatalog'
} else {
    $StaleCatalogFailure
}

function Assert-AACFrontMoov {
    param([Parameter(Mandatory)] [string]$Path)
    $bytes = [IO.File]::ReadAllBytes($Path)
    $types = [Collections.Generic.List[string]]::new()
    $offset = 0L
    for ($box = 0; $box -lt 32 -and $offset + 8 -le $bytes.LongLength; $box++) {
        $size = [uint64]$bytes[$offset] * 16777216 +
            [uint64]$bytes[$offset + 1] * 65536 +
            [uint64]$bytes[$offset + 2] * 256 +
            [uint64]$bytes[$offset + 3]
        $type = [Text.Encoding]::ASCII.GetString($bytes, [int]$offset + 4, 4)
        $headerBytes = 8L
        if ($size -eq 1) {
            if ($offset + 16 -gt $bytes.LongLength) { break }
            $size = 0
            for ($index = 8; $index -lt 16; $index++) {
                $size = ($size -shl 8) -bor $bytes[$offset + $index]
            }
            $headerBytes = 16L
        } elseif ($size -eq 0) {
            $size = [uint64]($bytes.LongLength - $offset)
        }
        if ($size -lt $headerBytes -or $size -gt [uint64]($bytes.LongLength - $offset)) {
            throw "AAC fixture has an invalid top-level '$type' box."
        }
        $types.Add($type)
        if ($type -eq 'mdat') { break }
        $offset += [int64]$size
    }
    $moovIndex = $types.IndexOf('moov')
    $mdatIndex = $types.IndexOf('mdat')
    if ($types.Count -eq 0 -or $types[0] -cne 'ftyp' -or
        $moovIndex -lt 0 -or $mdatIndex -lt 0 -or $moovIndex -ge $mdatIndex) {
        throw 'The catalog AAC fixture must use a front moov before mdat.'
    }
}

if ($Codec -ne 'ATMOS' -and $PSBoundParameters.ContainsKey('AtmosOutput')) {
    throw '-AtmosOutput requires -Codec ATMOS.'
}
if ([string]::IsNullOrWhiteSpace($FixturePath)) {
    $FixturePath = if ($Codec -eq 'ATMOS') {
        '.local/tests/atmos-48-progressive.ec3'
    } elseif ($SlowStreamTest) {
        if ($Codec -eq 'AAC') {
            '.local/tests/aac-48-progressive.m4a'
        } else {
            '.local/tests/alac-24-192-progressive.m4a'
        }
    } elseif ($Codec -eq 'AAC') {
        # Model the catalog's front-init contract without re-encoding the 3s AAC.
        # Keep tail-moov uploads as a separate
        # integration contract through the helper's uploaded-MP4 normalizer.
        '.local/tests/aac-48-faststart.m4a'
    } else {
        '.local/tests/alac-24-192-fragmented.m4a'
    }
}
$fixtureCandidate = Join-Path $Root $FixturePath
if (-not (Test-Path -LiteralPath $fixtureCandidate -PathType Leaf)) {
    if ($Codec -eq 'ATMOS') {
        throw "Atmos progressive fixture is missing: $FixturePath"
    } elseif ($SlowStreamTest) {
        throw "Slow-stream progressive fixture is missing: $FixturePath"
    }
    throw "Fixture is missing: $FixturePath"
}
$fixture = (Resolve-Path -LiteralPath $fixtureCandidate).Path
$testRoot = (Resolve-Path -LiteralPath (Join-Path $Root '.local/tests')).Path
if (-not $fixture.StartsWith($testRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The fixture must be inside this repository''s .local/tests directory.'
}
if ($Codec -eq 'AAC' -and $StaleCatalogTest) {
    Assert-AACFrontMoov -Path $fixture
}
$bridge = Join-Path $Root '.local/fb2k-dev/profile/glasswave/applemusic/bridge.json'
$savedBridge = if (Test-Path -LiteralPath $bridge) { [IO.File]::ReadAllBytes($bridge) } else { $null }
$mockBridge = Join-Path $testRoot 'applemusic-mock-bridge.json'
$mockState = Join-Path $testRoot 'applemusic-mock-state.json'
$resultPath = Join-Path $Root '.local/fb2k-dev/profile/glasswave/applemusic/probe-result.json'
$node = (& mise which node).Trim()
if ($LASTEXITCODE -ne 0) { throw 'The project Node runtime is unavailable.' }
$mock = Join-Path $Root 'tests/applemusic_mock.mjs'
if ($StaleCatalogTest) {
    $expectedSourceMetadata = [ordered]@{
        id = 'glasswave-test'
        catalogId = '1237064886'
        libraryId = 'i.GlasswaveStaleFixture'
        reportingId = ''
        durationMs = 3000
        title = 'Glasswave metadata title'
        album = ''
        artist = 'Glasswave metadata artist'
    }
}
Remove-Item -LiteralPath $mockBridge -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $mockState -Force -ErrorAction SilentlyContinue
$process = $null
$mockRunId = [Guid]::NewGuid().ToString('N')

function Wait-MockState {
    param(
        [Parameter(Mandatory)] [string]$Path,
        [Parameter(Mandatory)] [int]$ExpectedSessions,
        [Parameter(Mandatory)] [int]$ExpectedAudioGets,
        [int]$TimeoutSeconds = 15
    )
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (Test-Path -LiteralPath $Path -PathType Leaf) {
            try {
                $candidate = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json -ErrorAction Stop
                if ([int]$candidate.audioGetCount -gt $ExpectedAudioGets) {
                    throw "Audio GET count exceeded $ExpectedAudioGets."
                }
                if ([int]$candidate.playbackSessionCount -eq $ExpectedSessions -and
                    [int]$candidate.deleteCount -eq $ExpectedSessions -and
                    [int]$candidate.audioGetCount -eq $ExpectedAudioGets -and
                    [int]$candidate.activeSessionCount -eq 0) {
                    return $candidate
                }
            } catch {
                if ($_.Exception.Message -like 'Audio GET count exceeded*') { throw }
            }
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Mock counters did not reach the expected state: sessions=$ExpectedSessions, audioGets=$ExpectedAudioGets."
}

function Assert-FullProbeResult {
    param(
        [Parameter(Mandatory)] [object]$ProbeResult,
        [Parameter(Mandatory)] [string]$Label
    )
    if (-not $ProbeResult.ok) {
        throw "$Label failed: $($ProbeResult.error)."
    }
    if ([string]$ProbeResult.codec -notmatch "(?i)\b$Codec\b") {
        throw "$Label expected $Codec; got '$($ProbeResult.codec)'."
    }
    if (-not $ProbeResult.metadataVerified) {
        throw "$Label did not retain catalog title/artist metadata."
    }
    if (-not $ProbeResult.seekVerified) {
        throw "$Label did not pass the near-EOF seek probe."
    }
    if ($SeekStress -and [int]$ProbeResult.seekStressCount -ne 6) {
        throw "$Label did not complete all early seek transitions."
    }
    if ($Codec -eq 'AAC') {
        if ([int]$ProbeResult.sampleRate -ne 48000 -or
            [math]::Abs([double]$ProbeResult.samples - 144000.0) -gt 6000.0) {
            throw "$Label AAC format mismatch: rate=$($ProbeResult.sampleRate), samples=$($ProbeResult.samples)."
        }
    } elseif ($Codec -eq 'ATMOS') {
        $expectedChannels = switch ($AtmosOutput) {
            'StereoSpeakers' { 2 }
            'Headphones' { 2 }
            'Speakers51' { 6 }
            'Speakers71' { 8 }
        }
        $expectedMask = switch ($AtmosOutput) {
            'StereoSpeakers' { 0x000003 }
            'Headphones' { 0x000003 }
            'Speakers51' { 0x00060F }
            'Speakers71' { 0x00063F }
        }
        $expectedOutput = switch ($AtmosOutput) {
            'StereoSpeakers' { 'stereo speakers' }
            'Headphones' { 'headphones' }
            'Speakers51' { '5.1 speakers' }
            'Speakers71' { '7.1 speakers' }
        }
        if ([string]$ProbeResult.codec -notmatch '(?i)E-AC-3|EAC3' -or
            [string]$ProbeResult.codec -notmatch '(?i)JOC' -or
            -not $ProbeResult.atmosRendered -or
            [int]$ProbeResult.atmosObjects -ne 15 -or
            [int]$ProbeResult.sampleRate -ne 48000 -or
            [int]$ProbeResult.channels -ne $expectedChannels -or
            [uint32]$ProbeResult.channelMask -ne [uint32]$expectedMask -or
            [string]$ProbeResult.renderOutput -cne $expectedOutput) {
            throw "$Label Atmos contract failed."
        }
        if ([math]::Abs([double]$ProbeResult.decodedSeconds - 3.008) -gt 0.02 -or
            [math]::Abs([double]$ProbeResult.declaredSeconds - 3.0) -gt 0.05) {
            throw "$Label Atmos duration mismatch."
        }
    } else {
        if ([int]$ProbeResult.bitDepth -ne 24 -or
            [int]$ProbeResult.sampleRate -ne 192000 -or
            [uint64]$ProbeResult.samples -ne [uint64]576000) {
            throw "$Label ALAC format mismatch."
        }
    }
    if ($Codec -ne 'ATMOS' -and
        ([math]::Abs([double]$ProbeResult.decodedSeconds - 3.0) -gt 0.125 -or
        [math]::Abs([double]$ProbeResult.declaredSeconds - 3.0) -gt 0.125)) {
        throw "$Label decoded a non-three-second fixture."
    }
}

function Assert-StaleCatalogMetadata {
    param(
        [Parameter(Mandatory)] [object]$ProbeResult,
        [Parameter(Mandatory)] [System.Collections.IDictionary]$ExpectedSource,
        [switch]$DecodeFailed
    )
    if ([string]$ProbeResult.metadataInputPath -cne 'apple://song/glasswave-test') {
        throw 'The probe did not retain the original input identity.'
    }
    if (-not $ProbeResult.fixtureTransactionRestored) {
        throw 'The probe did not restore its prior transport transaction context.'
    }
    foreach ($key in $ExpectedSource.Keys) {
        if ([string]$ProbeResult.metadataSource.$key -cne [string]$ExpectedSource[$key]) {
            throw "The probe source catalog snapshot changed field '$key'."
        }
    }
    foreach ($key in @('id', 'catalogId', 'libraryId', 'reportingId')) {
        if ([string]$ProbeResult.metadataDisplay.$key -cne [string]$ExpectedSource[$key]) {
            throw "The probe display catalog snapshot changed identity '$key'."
        }
    }
    if ($DecodeFailed) { return }
    if (-not $ProbeResult.metadataVerified -or -not $ProbeResult.metadataIdentityVerified) {
        throw 'The probe did not verify original catalog identity and metadata.'
    }
    foreach ($key in @('title', 'artist', 'album')) {
        $expected = [string]$ProbeResult.metadataDisplay.$key
        if ($key -ne 'album' -and [string]::IsNullOrWhiteSpace($expected)) {
            throw "The display catalog snapshot has no '$key'."
        }
        if ($expected -cne '' -and
            ([string]$ProbeResult.metadataGetInfo.$key -cne $expected -or
             [string]$ProbeResult.metadataDynamicInfo.$key -cne $expected)) {
            throw "The decoder did not retain display catalog '$key'."
        }
    }
}

function Assert-MockSessions {
    param(
        [Parameter(Mandatory)] [object]$State,
        [Parameter(Mandatory)] [int]$ExpectedSessions,
        [Parameter(Mandatory)] [int]$ExpectedAudioGets,
        [Parameter(Mandatory)] [string]$ExpectedSourceVersion,
        [Parameter(Mandatory)] [string]$Label
    )
    $mappingMode = [string]$State.mappingMode
    $mappingModes = @(
        'Normal', 'StaleCatalog', 'InvalidSessionMetadata', 'PreviewRejected',
        'WrongLibrary', 'WrongReportingId'
    )
    if ($mappingMode -notin $mappingModes) {
        throw "$Label used unexpected mapping mode '$mappingMode'."
    }
    $hasStaleMapping = $mappingMode -ne 'Normal'
    $expectedFixture = 'glasswave-test'
    $expectedInitialAsset = if ($hasStaleMapping) { '1237064886' } else { 'glasswave-test' }
    $createsSession = $mappingMode -in @('Normal', 'StaleCatalog', 'InvalidSessionMetadata', 'PreviewRejected')
    $expectedSessionAsset = if ($mappingMode -eq 'Normal') {
        'glasswave-test'
    } elseif ($createsSession) {
        '1865440142'
    } else {
        $null
    }
    if ([string]$State.fixtureAssetId -cne $expectedFixture -or
        [string]$State.fixturePlaybackAssetId -cne $expectedInitialAsset -or
        [int]$State.playbackSessionCount -ne $ExpectedSessions -or
        [int]$State.audioGetCount -ne $ExpectedAudioGets -or
        [int]$State.deleteCount -ne $ExpectedSessions -or
        [int]$State.successfulDeleteCount -ne $ExpectedSessions -or
        [int]$State.deletedSessionCount -ne $ExpectedSessions -or
        [int]$State.activeSessionCount -ne 0 -or
        [int]$State.unexpectedDeleteCount -ne 0) {
        throw "$Label mock session/counter invariants failed."
    }
    if (@($State.rejectedBackgroundRequests | Where-Object { [int]$_.status -ne 403 }).Count -ne 0) {
        throw "$Label admitted a request outside the fixture transaction."
    }
    if ([string]$State.sourceVersion -cne $ExpectedSourceVersion -or
        $ExpectedSourceVersion -notmatch '^glasswave-mock-v1:[A-Za-z0-9_-]+$' -or
        @($State.sessionRecords).Count -ne $ExpectedSessions -or
        @($State.sessionRecords | Where-Object {
            [string]$_.sourceVersion -cne $ExpectedSourceVersion -or
            $_.deleted -ne $true -or
            [string]$_.assetId -cne $expectedSessionAsset
        }).Count -gt 0) {
        throw "$Label source-version or session DELETE coverage failed."
    }
    if ($hasStaleMapping) {
        $attempts = @($State.playbackAttempts)
        $expectedAttempts = if ($createsSession) { 2 } else { 1 }
        if ([int]$State.libraryLookupCount -ne 1 -or
            [string]$State.libraryId -cne 'i.GlasswaveStaleFixture' -or
            [string]$State.reportingId -cne '1865440142' -or
            @($State.libraryLookupPaths).Count -ne 1 -or
            -not ([string]$State.libraryLookupPaths[0]).StartsWith('/v1/me/library/songs/i.GlasswaveStaleFixture?', [StringComparison]::Ordinal) -or
            $attempts.Count -ne $expectedAttempts -or
            [string]$attempts[0] -cne '1237064886' -or
            ($createsSession -and [string]$attempts[1] -cne '1865440142') -or
            @($State.playbackAssetIds).Count -ne $(if ($createsSession) { $ExpectedSessions } else { 0 }) -or
            ($createsSession -and
                @($State.playbackAssetIds | Where-Object { [string]$_ -cne $expectedSessionAsset }).Count -gt 0) -or
            $State.authoritativeMappingReturned -ne $true -or
            $State.mappingAccepted -ne $createsSession) {
            throw "$Label stale-catalog mapping invariants failed."
        }
    }
    if ($Codec -eq 'ATMOS' -and
        ([string]$State.codec -cne 'ATMOS' -or $State.preferAtmos -ne $true)) {
        throw "$Label Atmos mock request did not use preferAtmos."
    }
}

function Copy-ProbeResultForEvidence {
    param(
        [Parameter(Mandatory)] [object]$ProbeResult
    )
    $names = @(
        'ok', 'codec', 'bitDepth', 'sampleRate', 'channels', 'samples',
        'decodedSeconds', 'declaredSeconds', 'firstDecodedMs', 'firstDecodedUnixMs',
        'canSeek', 'seekVerified', 'seekStressCount', 'metadataVerified', 'canceledAfterFirstChunk',
        'metadataIdentityVerified', 'metadataInputPath', 'metadataSource', 'metadataDisplay',
        'metadataGetInfo', 'metadataDynamicInfo', 'decoderError',
        'fixtureTransactionRestored',
        'shutdownMs', 'atmosRendered', 'atmosObjects', 'renderOutput',
        'channelMask', 'error'
    )
    $copy = [ordered]@{}
    foreach ($name in $names) {
        $property = $ProbeResult.PSObject.Properties[$name]
        if ($null -eq $property) {
            throw "Probe result is missing '$name' and cannot be recorded as evidence."
        }
        $copy[$name] = $property.Value
    }
    [pscustomobject]$copy
}

function Copy-MockStateForEvidence {
    param(
        [Parameter(Mandatory)] [object]$State
    )
    $sessions = @(
        foreach ($record in @($State.sessionRecords)) {
            [ordered]@{
                sessionId = $record.sessionId
                assetId = $record.assetId
                sourceVersion = $record.sourceVersion
                preferAtmos = $record.preferAtmos
                deleted = $record.deleted
            }
        }
    )
    [pscustomobject][ordered]@{
        mockRunId = $State.mockRunId
        sourceVersion = $State.sourceVersion
        fixtureAssetId = $State.fixtureAssetId
        fixturePlaybackAssetId = $State.fixturePlaybackAssetId
        libraryId = $State.libraryId
        reportingId = $State.reportingId
        mappingMode = $State.mappingMode
        libraryLookupPaths = @($State.libraryLookupPaths)
        playbackAttempts = @($State.playbackAttempts)
        rejectedAssetIds = @($State.rejectedAssetIds)
        rejectedBackgroundRequests = @($State.rejectedBackgroundRequests)
        authoritativeMappingReturned = $State.authoritativeMappingReturned
        mappingAccepted = $State.mappingAccepted
        codec = $State.codec
        preferAtmos = $State.preferAtmos
        audioGetCount = $State.audioGetCount
        playbackSessionCount = $State.playbackSessionCount
        activeSessionCount = $State.activeSessionCount
        deletedSessionCount = $State.deletedSessionCount
        deleteCount = $State.deleteCount
        successfulDeleteCount = $State.successfulDeleteCount
        unexpectedDeleteCount = $State.unexpectedDeleteCount
        playbackAssetIds = @($State.playbackAssetIds)
        firstAudioRead = $State.firstAudioRead
        serverDownloadFinished = $State.serverDownloadFinished
        clientDisconnected = $State.clientDisconnected
        sessionRecords = $sessions
    }
}

try {
    $mockArguments = @(
        ('"' + $mock + '"'),
        ('"' + $fixture + '"'),
        ('"' + $mockBridge + '"'),
        ('"' + $mockState + '"'),
        $Codec,
        $(if ($SlowStreamTest) { 'SlowStreamTest' } else { 'Normal' }),
        $mockRunId,
        $mappingMode
    )
    $process = Start-Process -FilePath $node -ArgumentList $mockArguments -WindowStyle Hidden -PassThru
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    while (-not (Test-Path -LiteralPath $mockBridge)) {
        if ($process.HasExited -or [DateTime]::UtcNow -gt $deadline) {
            throw 'The Apple Music fixture server did not start.'
        }
        Start-Sleep -Milliseconds 100
    }
    Copy-Item -LiteralPath $mockBridge -Destination $bridge -Force
    $probeArguments = @{
        SongId = 'glasswave-test'
        FixtureMapping = [bool]$StaleCatalogTest
        VerifyMetadata = $true
        Deploy = $true
        TimeoutSeconds = 30
        CancelStreamTest = [bool]$CancelStreamTest
        SeekStress = [bool]$SeekStress
    }
    if ($Codec -eq 'ATMOS') {
        $probeArguments.Atmos = $true
        $probeArguments.AtmosOutput = $AtmosOutput
    }
    & (Join-Path $PSScriptRoot 'probe-applemusic.ps1') @probeArguments
    $probeExitCode = $LASTEXITCODE
    $result = if (Test-Path -LiteralPath $resultPath -PathType Leaf) {
        Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json -ErrorAction Stop
    } else {
        $null
    }
    $expectProbeFailure = $StaleCatalogFailure -ne 'None'
    if ($expectProbeFailure) {
        if ($probeExitCode -eq 0 -or $null -eq $result -or $result.ok) {
            throw "The $StaleCatalogFailure fixture unexpectedly completed playback."
        }
        $expectedFailure = switch ($StaleCatalogFailure) {
            'InvalidSessionMetadata' { 'invalid full-song session' }
            'PreviewRejected' { 'incomplete full-song session' }
            'WrongLibrary' { 'HTTP 404' }
            'WrongReportingId' { 'HTTP 404' }
        }
        if ([string]$result.error -cne 'decoder_initialize_failed' -or
            [string]$result.decoderError -notmatch [regex]::Escape($expectedFailure)) {
            throw "The $StaleCatalogFailure fixture returned the wrong error: $($result.error): $($result.decoderError)"
        }
    } elseif ($probeExitCode -ne 0 -or $null -eq $result -or -not $result.ok) {
        throw 'The encoded transport probe failed.'
    }
    if ($StaleCatalogTest) {
        Assert-StaleCatalogMetadata -ProbeResult $result -ExpectedSource $expectedSourceMetadata `
            -DecodeFailed:$expectProbeFailure
    }
    if (-not $expectProbeFailure) {
        if ([string]$result.codec -notmatch "(?i)\b$Codec\b") {
            throw "Expected $Codec; got '$($result.codec)' ($($result.error))."
        }
        if (-not $result.metadataVerified) {
            throw 'Catalog title/artist metadata was not retained by get_info and first get_dynamic_info.'
        }
    if ($Codec -eq 'ATMOS') {
        if ([string]$result.codec -notmatch '(?i)E-AC-3|EAC3' -or
            [string]$result.codec -notmatch '(?i)JOC' -or
            -not $result.atmosRendered -or
            [int]$result.atmosObjects -ne 15) {
            throw "Atmos renderer contract failed: codec='$($result.codec)', rendered=$($result.atmosRendered), objects=$($result.atmosObjects)."
        }
        $expectedChannels = switch ($AtmosOutput) {
            'StereoSpeakers' { 2 }
            'Headphones' { 2 }
            'Speakers51' { 6 }
            'Speakers71' { 8 }
        }
        $expectedMask = switch ($AtmosOutput) {
            'StereoSpeakers' { 0x000003 }
            'Headphones' { 0x000003 }
            'Speakers51' { 0x00060F }
            'Speakers71' { 0x00063F }
        }
        $expectedOutput = switch ($AtmosOutput) {
            'StereoSpeakers' { 'stereo speakers' }
            'Headphones' { 'headphones' }
            'Speakers51' { '5.1 speakers' }
            'Speakers71' { '7.1 speakers' }
        }
        if ([int]$result.channels -ne $expectedChannels -or
            [uint32]$result.channelMask -ne [uint32]$expectedMask -or
            [string]$result.renderOutput -cne $expectedOutput) {
            throw "Atmos render output mismatch: channels=$($result.channels), mask=$($result.channelMask), output='$($result.renderOutput)'."
        }
    }
    if (-not $CancelStreamTest) {
        if ($Codec -eq 'AAC') {
            if ([int]$result.sampleRate -ne 48000) {
                throw "Expected 48 kHz AAC; got $($result.sampleRate) Hz."
            }
            if ([math]::Abs([double]$result.samples - 144000.0) -gt 6000.0) {
                throw "Expected approximately 144000 AAC sample frames; got $($result.samples)."
            }
        } elseif ($Codec -eq 'ATMOS') {
            if ([int]$result.sampleRate -ne 48000 -or [int]$result.channels -ne $expectedChannels) {
                throw "Expected 48 kHz Atmos output with $expectedChannels channels."
            }
            if ([math]::Abs([double]$result.decodedSeconds - 3.008) -gt 0.02 -or
                [math]::Abs([double]$result.declaredSeconds - 3.0) -gt 0.05) {
                throw "Expected a 3.008 second Atmos fixture; decoded=$($result.decodedSeconds), declared=$($result.declaredSeconds)."
            }
        } else {
            if ([int]$result.bitDepth -ne 24 -or [int]$result.sampleRate -ne 192000) {
                throw "Expected 24-bit/192 kHz; got $($result.bitDepth)-bit/$($result.sampleRate) Hz."
            }
            if ([uint64]$result.samples -ne [uint64]576000) {
                throw "Expected 576000 sample frames; got $($result.samples)."
            }
        }
        if (-not $result.seekVerified) {
            throw 'The completed spool did not pass the near-EOF seek probe.'
        }
        if ($Codec -ne 'ATMOS' -and
            ([math]::Abs([double]$result.decodedSeconds - 3.0) -gt 0.125 -or
            [math]::Abs([double]$result.declaredSeconds - 3.0) -gt 0.125)) {
            throw "Expected a 3 second fixture; decoded=$($result.decodedSeconds), declared=$($result.declaredSeconds)."
        }
    }
    if ($CancelStreamTest) {
        if (-not $result.canceledAfterFirstChunk) {
            throw 'The probe did not cancel immediately after the first decoded chunk.'
        }
        if ($null -eq $result.shutdownMs -or [double]$result.shutdownMs -ge 2000) {
            throw "Decoder shutdown exceeded 2000 ms: $($result.shutdownMs)"
        }
    }
    }
    $expectedSessions = if ($StaleCatalogFailure -in @('WrongLibrary', 'WrongReportingId')) {
        0
    } else {
        1
    }
    $expectedAudioGets = if ($StaleCatalogFailure -in @('InvalidSessionMetadata', 'PreviewRejected') -or
        $StaleCatalogFailure -in @('WrongLibrary', 'WrongReportingId')) {
        0
    } else {
        1
    }
    $state = Wait-MockState -Path $mockState -ExpectedSessions $expectedSessions `
        -ExpectedAudioGets $expectedAudioGets
    Assert-MockSessions -State $state -ExpectedSessions $expectedSessions `
        -ExpectedAudioGets $expectedAudioGets `
        -ExpectedSourceVersion ([string]$state.sourceVersion) -Label 'First probe'
    $expectedPlaybackAsset = if ($StaleCatalogTest) {
        '1865440142'
    } else {
        'glasswave-test'
    }
    if ([string]$state.fixtureAssetId -cne 'glasswave-test' -or
        @($state.playbackAssetIds).Count -ne $expectedSessions -or
        @($state.playbackAssetIds | Where-Object { [string]$_ -cne $expectedPlaybackAsset }).Count -gt 0) {
        throw 'The decoder test attempted an unexpected playback asset ID.'
    }
    if ($Codec -eq 'ATMOS' -and
        ([string]$state.codec -cne 'ATMOS' -or $state.preferAtmos -ne $true)) {
        throw 'Atmos mock request did not use the guarded preferAtmos fixture path.'
    }
    if ([int]$state.deleteCount -ne $expectedSessions) {
        throw 'The input did not DELETE each created playback session exactly once.'
    }
    if ($SlowStreamTest) {
        if ($null -eq $state.firstAudioRead -or $null -eq $result.firstDecodedUnixMs) {
            throw 'Slow-stream timing evidence is incomplete.'
        }
        $firstAudioRead = [double]$state.firstAudioRead
        $firstDecoded = [double]$result.firstDecodedUnixMs
        if ($CancelStreamTest) {
            if ($null -ne $state.serverDownloadFinished -or $null -eq $state.clientDisconnected) {
                throw 'Cancellation did not abort the slow media response before EOF.'
            }
            $clientDisconnected = [double]$state.clientDisconnected
            if ($firstDecoded -lt $firstAudioRead -or $firstDecoded -gt $clientDisconnected) {
                throw "First audio was not decoded before the canceled stream disconnected: request=$firstAudioRead, decoded=$firstDecoded, disconnected=$clientDisconnected."
            }
        } else {
            if ($null -eq $state.serverDownloadFinished) {
                throw 'Slow-stream timing evidence is incomplete.'
            }
            $downloadFinished = [double]$state.serverDownloadFinished
            if ($firstDecoded -lt $firstAudioRead -or $firstDecoded -ge $downloadFinished) {
                throw "First audio was not decoded before the slow media response completed: request=$firstAudioRead, decoded=$firstDecoded, finished=$downloadFinished."
            }
        }
        $timingName = 'applemusic-{0}-{1}-timing.json' -f $Codec.ToLowerInvariant(), $(if ($CancelStreamTest) { 'cancel' } else { 'slow' })
        [ordered]@{
            codec = $Codec
            firstAudioReadUnixMs = $state.firstAudioRead
            firstDecodedUnixMs = $result.firstDecodedUnixMs
            transferFinishedUnixMs = $state.serverDownloadFinished
            clientDisconnectedUnixMs = $state.clientDisconnected
            firstAudioBeforeEofVerified = $true
            canceled = [bool]$CancelStreamTest
            shutdownMs = $result.shutdownMs
            sessionDeletes = $state.deleteCount
        } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $testRoot $timingName) -Encoding utf8
    }
    $firstResult = $result
    $firstState = $state
    if ($StaleCatalogTest) {
        $caseName = if ($StaleCatalogFailure -eq 'None') { 'full' } else { $StaleCatalogFailure.ToLowerInvariant() }
        [ordered]@{
            schema = 'glasswave-applemusic-stale-catalog-v1'
            case = $caseName
            codec = $Codec
            expectedSuccess = -not $expectProbeFailure
            result = $result
            mockState = Copy-MockStateForEvidence -State $state
        } | ConvertTo-Json -Depth 8 |
            Set-Content -LiteralPath (Join-Path $testRoot "applemusic-stale-catalog-$caseName-$($Codec.ToLowerInvariant()).json") `
                -Encoding utf8
    }
    if ($CacheReuseTest) {
        Assert-FullProbeResult -ProbeResult $firstResult -Label 'First cache-reuse probe'

        # Keep the fixture server and its counters alive across the second startup probe.
        & (Join-Path $PSScriptRoot 'probe-applemusic.ps1') @probeArguments
        if ($LASTEXITCODE -ne 0) { throw 'The cache-reuse probe failed.' }
        $secondResult = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json -ErrorAction Stop
        Assert-FullProbeResult -ProbeResult $secondResult -Label 'Second cache-reuse probe'

        $expectedSourceVersion = [string]$firstState.sourceVersion
        $finalState = Wait-MockState -Path $mockState -ExpectedSessions 2 -ExpectedAudioGets 1
        Assert-MockSessions -State $finalState -ExpectedSessions 2 -ExpectedAudioGets 1 `
            -ExpectedSourceVersion $expectedSourceVersion -Label 'Cache reuse'
        if ([string]$finalState.mockRunId -cne [string]$firstState.mockRunId -or
            [int]$finalState.audioGetCount -ne [int]$firstState.audioGetCount -or
            [int]$finalState.audioGetCount -ne 1) {
            throw 'Cache hit count or mock run identity changed across the two probes.'
        }

        $evidenceName = 'applemusic-{0}-cache-reuse.json' -f $Codec.ToLowerInvariant()
        [ordered]@{
            schema = 'glasswave-applemusic-cache-reuse-v1'
            codec = $Codec
            atmosOutput = if ($Codec -eq 'ATMOS') { $AtmosOutput } else { $null }
            slowStreamTest = [bool]$SlowStreamTest
            assertions = [ordered]@{
                firstFullDecode = $true
                firstMetadataVerified = [bool]$firstResult.metadataVerified
                firstSeekVerified = [bool]$firstResult.seekVerified
                secondFullDecode = $true
                secondMetadataVerified = [bool]$secondResult.metadataVerified
                secondSeekVerified = [bool]$secondResult.seekVerified
                audioGetCount = [int]$finalState.audioGetCount
                secondAudioGetCount = [int]$finalState.audioGetCount - [int]$firstState.audioGetCount
                playbackSessionCount = [int]$finalState.playbackSessionCount
                deleteCount = [int]$finalState.deleteCount
                successfulDeleteCount = [int]$finalState.successfulDeleteCount
                activeSessionCount = [int]$finalState.activeSessionCount
                unexpectedDeleteCount = [int]$finalState.unexpectedDeleteCount
                sameSourceVersion = $true
                allSessionsDeleted = $true
            }
            firstResult = Copy-ProbeResultForEvidence -ProbeResult $firstResult
            secondResult = Copy-ProbeResultForEvidence -ProbeResult $secondResult
            firstMockState = Copy-MockStateForEvidence -State $firstState
            finalMockState = Copy-MockStateForEvidence -State $finalState
        } | ConvertTo-Json -Depth 8 |
            Set-Content -LiteralPath (Join-Path $testRoot $evidenceName) -Encoding utf8
    }
} finally {
    if ($process -and -not $process.HasExited) { $process.Kill(); [void]$process.WaitForExit(3000) }
    if ($null -ne $savedBridge) { [IO.File]::WriteAllBytes($bridge, $savedBridge) }
    else { Remove-Item -LiteralPath $bridge -Force -ErrorAction SilentlyContinue }
    Remove-Item -LiteralPath $mockBridge -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $mockState -Force -ErrorAction SilentlyContinue
}
