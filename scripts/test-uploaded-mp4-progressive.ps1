#Requires -Version 7.2
<#
.SYNOPSIS
    Exercise uploaded-MP4 progressive normalization through the real Apple input.
#>
[CmdletBinding()]
param(
    [string]$FixtureName = 'aac-48.m4a',
    [switch]$UseShortAACFixture,
    [string]$GoEnginePath = '',
    [ValidateRange(1, 15)]
    [int]$PreReleaseDelaySeconds = 6,
    [ValidateRange(30, 180)]
    [int]$ProbeTimeoutSeconds = 120
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Root = Split-Path -Parent $PSScriptRoot
$TestRoot = Join-Path $Root '.local/tests'
$Instance = Join-Path $Root '.local/fb2k-dev'
$RepoExe = Join-Path $Instance 'foobar2000.exe'
$Bridge = Join-Path $Instance 'profile/glasswave/applemusic/bridge.json'
$ResultPath = Join-Path $Instance 'profile/glasswave/applemusic/probe-result.json'
$StageEngine = if ([string]::IsNullOrWhiteSpace($GoEnginePath)) {
    Join-Path $Root '.local/applemusic/windows/stage/adam-nightly-headless/engine'
} else {
    [IO.Path]::GetFullPath($GoEnginePath)
}
$CanonicalEngine = Join-Path $Root '.local/applemusic/adam-nightly/engine'
$Mock = Join-Path $Root 'tests/applemusic_mock.mjs'
$Probe = Join-Path $PSScriptRoot 'probe-applemusic.ps1'
$Stamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
$Evidence = Join-Path $TestRoot "applemusic-uploaded-mp4-progressive-$Stamp"
$MockBridge = Join-Path $Evidence 'mock-bridge.json'
$MockState = Join-Path $Evidence 'mock-state.json'
$FixtureState = Join-Path $Evidence 'fixture-state.json'

if (-not (Test-Path -LiteralPath $RepoExe -PathType Leaf)) { throw 'Repository dev instance is missing.' }
if (-not (Test-Path -LiteralPath $StageEngine -PathType Container)) { throw 'Pinned source stage is missing.' }
if (-not (Test-Path -LiteralPath $CanonicalEngine -PathType Container)) { throw 'Canonical Go source is missing.' }
if (-not (Test-Path -LiteralPath $Mock -PathType Leaf)) { throw 'Apple Music mock server is missing.' }
if (-not (Test-Path -LiteralPath $Probe -PathType Leaf)) { throw 'Apple Music probe script is missing.' }
if (-not (Test-Path -LiteralPath $Bridge -PathType Leaf)) { throw 'Real bridge configuration is missing.' }

$repoProcesses = @(
    Get-Process -Name foobar2000 -ErrorAction SilentlyContinue |
        Where-Object {
            try {
                $_.Path -and [string]::Equals(
                    [IO.Path]::GetFullPath($_.Path),
                    [IO.Path]::GetFullPath($RepoExe),
                    [StringComparison]::OrdinalIgnoreCase)
            } catch { $false }
        }
)
if ($repoProcesses.Count -ne 0) {
    throw 'The repository dev instance must be stopped before the exclusive native test.'
}

New-Item -ItemType Directory -Path $Evidence -Force | Out-Null
$fixtureName = $FixtureName.Trim()
if ($fixtureName -ne [IO.Path]::GetFileName($fixtureName)) {
    throw 'FixtureName must name a file directly under .local/tests.'
}
$fixturePath = Join-Path $TestRoot $fixtureName
if (-not (Test-Path -LiteralPath $fixturePath -PathType Leaf)) {
    throw "Fixture is missing: $fixturePath"
}

function Start-RedirectedProcess {
    param(
        [Parameter(Mandatory)] [string]$FilePath,
        [Parameter(Mandatory)] [string[]]$Arguments,
        [Parameter(Mandatory)] [string]$WorkingDirectory,
        [Parameter(Mandatory)] [string]$StdoutPath,
        [Parameter(Mandatory)] [string]$StderrPath,
        [hashtable]$Environment = @{}
    )
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $FilePath
    $info.WorkingDirectory = $WorkingDirectory
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
    foreach ($entry in $Environment.GetEnumerator()) { $info.Environment[$entry.Key] = [string]$entry.Value }
    $process = [Diagnostics.Process]::Start($info)
    [pscustomobject]@{
        Process = $process
        StdoutPath = $StdoutPath
        StderrPath = $StderrPath
        StdoutTask = $process.StandardOutput.ReadToEndAsync()
        StderrTask = $process.StandardError.ReadToEndAsync()
    }
}

function Save-ProcessLogs {
    param($Owned)
    if ($null -eq $Owned) { return }
    [IO.File]::WriteAllText($Owned.StdoutPath, $Owned.StdoutTask.Result, [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText($Owned.StderrPath, $Owned.StderrTask.Result, [Text.UTF8Encoding]::new($false))
}

function Stop-OwnedProcess {
    param($Owned)
    if ($null -eq $Owned) { return }
    if (-not $Owned.Process.HasExited) {
        $Owned.Process.Kill()
        [void]$Owned.Process.WaitForExit(5000)
    }
    Save-ProcessLogs $Owned
}

function Wait-ForFile {
    param([string]$Path, [int]$Seconds = 20)
    $deadline = [DateTime]::UtcNow.AddSeconds($Seconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        if (Test-Path -LiteralPath $Path -PathType Leaf) { return }
        Start-Sleep -Milliseconds 100
    }
    throw "Timed out waiting for $Path"
}

function Get-FixtureState {
    Invoke-RestMethod -Method Get -Uri "$($script:fixtureBase)/state" `
        -Headers @{ 'X-Glasswave-Fixture' = $script:fixtureToken } -TimeoutSec 10
}

function Invoke-FixturePost {
    param([string]$Path)
    [void](Invoke-WebRequest -Method Post -Uri "$($script:fixtureBase)$Path" `
        -Headers @{ 'X-Glasswave-Fixture' = $script:fixtureToken } -TimeoutSec 10)
}

function Start-Probe {
    param([switch]$SeekStress, [string]$Name)
    $arguments = @(
        '-NoLogo', '-NoProfile', '-NonInteractive', '-File', $Probe,
        '-SongId', 'glasswave-test', '-VerifyMetadata', '-Deploy',
        '-TimeoutSeconds', [string]$ProbeTimeoutSeconds
    )
    if ($SeekStress) { $arguments += '-SeekStress' }
    Start-RedirectedProcess -FilePath (Get-Process -Id $PID).Path -Arguments $arguments `
        -WorkingDirectory $Root `
        -StdoutPath (Join-Path $Evidence "$Name.stdout.log") `
        -StderrPath (Join-Path $Evidence "$Name.stderr.log")
}

function Wait-Probe {
    param($Owned, [string]$Name)
    if (-not $Owned.Process.WaitForExit(($ProbeTimeoutSeconds + 20) * 1000)) {
        Stop-OwnedProcess $Owned
        throw "$Name exceeded its host deadline."
    }
    Save-ProcessLogs $Owned
    if ($Owned.Process.ExitCode -ne 0) { throw "$Name failed: $($Owned.StderrTask.Result)" }
    if (-not (Test-Path -LiteralPath $ResultPath -PathType Leaf)) { throw "$Name produced no result." }
    Get-Content -LiteralPath $ResultPath -Raw | ConvertFrom-Json -ErrorAction Stop
}

function Assert-AACResult {
    param($Result, [string]$Label, [switch]$RequireSeekStress)
    if (-not $Result.ok -or [string]$Result.codec -notmatch '(?i)AAC') {
        throw "$Label did not complete AAC playback."
    }
    if ([int]$Result.sampleRate -ne 48000 -or
        [math]::Abs([double]$Result.decodedSeconds - [double]$Result.declaredSeconds) -gt 0.125) {
        throw "$Label reported inconsistent AAC format or duration."
    }
    if (-not $Result.seekVerified) { throw "$Label did not pass the near-EOF seek." }
    if ($RequireSeekStress -and [int]$Result.seekStressCount -ne 6) {
        throw "$Label did not complete all six early seeks."
    }
}

$goOutput = & mise exec go@1.26.0 -- where.exe go
$goExit = $LASTEXITCODE
$go = ($goOutput | Select-Object -First 1).Trim()
if ($goExit -ne 0 -or [string]::IsNullOrWhiteSpace($go)) { throw 'Pinned Go is unavailable.' }
$fixtureGeneration = [ordered]@{
    sourceName = $fixtureName
    sourceBytes = (Get-Item -LiteralPath $fixturePath).Length
    normalized = $false
}
if (-not $UseShortAACFixture) {
    $ffmpeg = (Get-Command ffmpeg.exe -ErrorAction Stop).Source
    $sourceFixturePath = $fixturePath
    $loopedName = "aac-48-long-unnormalized-$Stamp.m4a"
    $loopedPath = Join-Path $TestRoot $loopedName
    $fixtureName = "aac-48-long-normalized-$Stamp.m4a"
    $fixturePath = Join-Path $TestRoot $fixtureName

    # Repeat the encoded track without decoding. The source is long enough to
    # keep more than 25% of the original mdat behind the native midpoint gate.
    & $ffmpeg -hide_banner -loglevel error -y -stream_loop 83 -i $sourceFixturePath `
        -map 0 -c copy -t 252.757333 $loopedPath
    if ($LASTEXITCODE -ne 0) { throw 'Long AAC fixture generation failed.' }

    $normalizer = Start-RedirectedProcess -FilePath $go -Arguments @(
        'test', '-count=1', '-run', '^TestNormalizeUploadedMP4AACFixture$', './core/apple'
    ) -WorkingDirectory $CanonicalEngine `
        -StdoutPath (Join-Path $Evidence 'fixture-normalizer.stdout.log') `
        -StderrPath (Join-Path $Evidence 'fixture-normalizer.stderr.log') `
        -Environment @{
            CGO_ENABLED = '0'
            GOOS = 'windows'
            GOARCH = 'amd64'
            GOPROXY = 'off'
            GOSUMDB = 'off'
            GOTOOLCHAIN = 'local'
            GLASSWAVE_UPLOAD_MP4_NORMALIZE_INPUT = $loopedPath
            GLASSWAVE_UPLOAD_MP4_NORMALIZE_OUTPUT = $fixturePath
        }
    if (-not $normalizer.Process.WaitForExit(60000)) {
        Stop-OwnedProcess $normalizer
        throw 'AAC fixture timing normalization exceeded 60 seconds.'
    }
    Save-ProcessLogs $normalizer
    if ($normalizer.Process.ExitCode -ne 0) {
        throw "AAC fixture timing normalization failed: $($normalizer.StderrTask.Result)"
    }
    if (-not (Test-Path -LiteralPath $fixturePath -PathType Leaf)) {
        throw 'AAC fixture timing normalization produced no output.'
    }
    $generatedBytes = (Get-Item -LiteralPath $fixturePath).Length
    if ($generatedBytes -lt 7MB) {
        throw "Normalized AAC fixture is too short for the held-tail gate: $generatedBytes bytes."
    }
    $fixtureGeneration.loopedName = $loopedName
    $fixtureGeneration.loopedBytes = (Get-Item -LiteralPath $loopedPath).Length
    $fixtureGeneration.generatedBytes = $generatedBytes
    $fixtureGeneration.normalized = $true
}
$nodeOutput = & mise which node
$nodeExit = $LASTEXITCODE
$node = $nodeOutput.Trim()
if ($nodeExit -ne 0 -or [string]::IsNullOrWhiteSpace($node)) { throw 'Project Node is unavailable.' }
$pwsh = (Get-Process -Id $PID).Path
$savedBridge = [IO.File]::ReadAllBytes($Bridge)

$goFixture = $null
$nodeProxy = $null
$firstProbe = $null
$secondProbe = $null
$fixture = $null
$summary = $null

try {
    $goFixture = Start-RedirectedProcess -FilePath $go -Arguments @(
        'test', '-count=1', '-v', '-tags', 'adamplay_backend glasswave',
        '-run', '^TestUploadedMP4NativeFixture$', './core/apple'
    ) -WorkingDirectory $StageEngine `
        -StdoutPath (Join-Path $Evidence 'go-fixture.stdout.log') `
        -StderrPath (Join-Path $Evidence 'go-fixture.stderr.log') `
        -Environment @{
            CGO_ENABLED = '0'
            GOOS = 'windows'
            GOARCH = 'amd64'
            GLASSWAVE_UPLOAD_MP4_FIXTURE_STATE = $FixtureState
            GLASSWAVE_UPLOAD_MP4_FIXTURE_FILE = $fixtureName
        }
    Wait-ForFile $FixtureState 30
    $fixture = Get-Content -LiteralPath $FixtureState -Raw | ConvertFrom-Json -ErrorAction Stop
    foreach ($name in 'url', 'token', 'sourceVersion', 'outputBytes', 'originalBytes', 'originalMdatBytes', 'heldRemainingBytes') {
        if ($null -eq $fixture.$name) { throw "Native fixture state is missing '$name'." }
    }
    $minimumHeldBytes = [math]::Max(
        16KB,
        [int64]([math]::Floor([double]$fixture.originalMdatBytes / 4)))
    if ([int64]$fixture.heldRemainingBytes -lt $minimumHeldBytes) {
        throw 'Native fixture does not hold a substantial unreceived media tail.'
    }
    $script:fixtureBase = [string]$fixture.url
    $script:fixtureToken = [string]$fixture.token
    $cacheSourceVersion = "$([string]$fixture.sourceVersion):lease-$Stamp"

    $nodeProxy = Start-RedirectedProcess -FilePath $node -Arguments @(
        $Mock, $fixturePath, $MockBridge, $MockState,
        'AAC', 'Normal', "uploaded-progressive-$Stamp"
    ) -WorkingDirectory $Root `
        -StdoutPath (Join-Path $Evidence 'node-proxy.stdout.log') `
        -StderrPath (Join-Path $Evidence 'node-proxy.stderr.log') `
        -Environment @{
            GLASSWAVE_UPLOAD_MP4_UPSTREAM_URL = "$($script:fixtureBase)/stream"
            GLASSWAVE_UPLOAD_MP4_UPSTREAM_TOKEN = $script:fixtureToken
            GLASSWAVE_UPLOAD_MP4_SOURCE_VERSION = $cacheSourceVersion
            GLASSWAVE_UPLOAD_MP4_DURATION_MS = [string]$fixture.durationMs
        }
    Wait-ForFile $MockBridge 20
    [IO.File]::WriteAllBytes($Bridge, [IO.File]::ReadAllBytes($MockBridge))

    $firstProbe = Start-Probe -Name 'first-normal'
    $gateDeadline = [DateTime]::UtcNow.AddSeconds(30)
    $gateState = $null
    while ([DateTime]::UtcNow -lt $gateDeadline) {
        if ($firstProbe.Process.HasExited) {
            throw "First normal probe exited before the EOF gate: $($firstProbe.StderrTask.Result)"
        }
        try { $gateState = Get-FixtureState } catch {}
        if ($gateState -and [int64]$gateState.gateReachedUnixMs -gt 0) { break }
        Start-Sleep -Milliseconds 100
    }
    if ($null -eq $gateState -or [int64]$gateState.gateReachedUnixMs -le 0) {
        throw 'The normalized upstream did not reach the held final byte.'
    }

    Start-Sleep -Seconds $PreReleaseDelaySeconds
    Invoke-FixturePost '/release'
    $releasedState = Get-FixtureState
    if (-not $releasedState.released -or [int64]$releasedState.releaseUnixMs -le 0) {
        throw 'The native fixture did not record its release timestamp.'
    }
    $firstResult = Wait-Probe $firstProbe 'First normal probe'
    Assert-AACResult $firstResult 'First normal probe'
    $fixtureDeadline = [DateTime]::UtcNow.AddSeconds(10)
    do {
        $finalFixtureState = Get-FixtureState
        if ([int]$finalFixtureState.completed -gt 0 -or
            [int]$finalFixtureState.failures -gt 0) {
            break
        }
        if ([DateTime]::UtcNow -ge $fixtureDeadline) {
            throw 'The native fixture did not finish recording stream completion.'
        }
        Start-Sleep -Milliseconds 100
    } while ($true)
    if ($null -eq $firstResult.firstDecodedUnixMs -or
        [double]$firstResult.firstDecodedUnixMs -ge [double]$finalFixtureState.releaseUnixMs) {
        throw 'AAC decoding did not begin before the held final upstream byte was released.'
    }
    if ([int64]$finalFixtureState.lastByteWrittenUnixMs -gt 0 -and
        [int64]$finalFixtureState.lastByteWrittenUnixMs -lt
            [int64]$finalFixtureState.releaseUnixMs) {
        throw 'The final upstream byte was released before the recorded release action.'
    }
    if (-not $finalFixtureState.completed -or [int]$finalFixtureState.failures -ne 0) {
        throw 'The native normalizer did not complete its stream successfully.'
    }

    $firstMockState = Get-Content -LiteralPath $MockState -Raw | ConvertFrom-Json -ErrorAction Stop
    $secondProbe = Start-Probe -SeekStress -Name 'second-cache-seek'
    $secondResult = Wait-Probe $secondProbe 'Second cached seek probe'
    Assert-AACResult $secondResult 'Second cached seek probe' -RequireSeekStress
    $secondMockState = Get-Content -LiteralPath $MockState -Raw | ConvertFrom-Json -ErrorAction Stop

    if ([int]$secondMockState.audioGetCount -ne 1 -or
        [int]$secondMockState.audioGetCount -ne [int]$firstMockState.audioGetCount) {
        throw 'The cached second source performed another audio GET.'
    }
    if ([string]$secondMockState.sourceVersion -cne [string]$firstMockState.sourceVersion -or
        [string]$secondMockState.sourceVersion -cne $cacheSourceVersion -or
        -not ([string]$secondMockState.sourceVersion).StartsWith(
            [string]$fixture.sourceVersion + ':', [StringComparison]::Ordinal)) {
        throw 'The cached source version changed across probes.'
    }
    if (@($secondMockState.sessionRecords).Count -ne 2 -or
        [int]$secondMockState.deleteCount -ne 2) {
        throw 'Playback sessions were not created and deleted once per probe.'
    }

    $summary = [ordered]@{
        schema = 'glasswave-uploaded-mp4-progressive-v1'
        ok = $true
        fixture = [ordered]@{
            name = $fixtureName
            codec = 'AAC'
            generation = $fixtureGeneration
            outputBytes = [int64]$fixture.outputBytes
            originalBytes = [int64]$fixture.originalBytes
            originalMdatBytes = [int64]$fixture.originalMdatBytes
            gateAt = [int64]$fixture.gateAt
            heldRemainingBytes = [int64]$fixture.heldRemainingBytes
            sourceVersion = [string]$fixture.sourceVersion
            gateReachedUnixMs = [int64]$finalFixtureState.gateReachedUnixMs
            releaseUnixMs = [int64]$finalFixtureState.releaseUnixMs
            lastByteWrittenUnixMs = [int64]$finalFixtureState.lastByteWrittenUnixMs
            requests = [int]$finalFixtureState.requests
            completed = [int]$finalFixtureState.completed
            failures = [int]$finalFixtureState.failures
        }
        firstProbe = $firstResult
        secondProbe = $secondResult
        timing = [ordered]@{
            firstDecodedUnixMs = [double]$firstResult.firstDecodedUnixMs
            releaseUnixMs = [int64]$finalFixtureState.releaseUnixMs
            lastByteWrittenUnixMs = [int64]$finalFixtureState.lastByteWrittenUnixMs
            releaseResolution = 'milliseconds; final byte may share the release millisecond'
            gateReachedBeforeRelease = $true
            decodedBeforeFinalByte = $true
        }
        cache = [ordered]@{
            sourceVersion = [string]$secondMockState.sourceVersion
            sameVersion = $true
            audioGetCount = [int]$secondMockState.audioGetCount
            secondProbeExtraAudioGet = $false
            sessions = @($secondMockState.sessionRecords).Count
            deletes = [int]$secondMockState.deleteCount
        }
    }
}
finally {
    $appProcesses = @(
        Get-Process -Name foobar2000 -ErrorAction SilentlyContinue |
            Where-Object {
                try {
                    $_.Path -and [string]::Equals(
                        [IO.Path]::GetFullPath($_.Path),
                        [IO.Path]::GetFullPath($RepoExe),
                        [StringComparison]::OrdinalIgnoreCase)
                } catch { $false }
            }
    )
    foreach ($process in $appProcesses) {
        if (-not $process.HasExited) {
            $process.Kill()
            [void]$process.WaitForExit(5000)
        }
    }

    [IO.File]::WriteAllBytes($Bridge, $savedBridge)
    if ($null -ne $goFixture -and -not $goFixture.Process.HasExited -and $null -ne $fixture) {
        try { Invoke-FixturePost '/stop' } catch {}
    }
    Stop-OwnedProcess $firstProbe
    Stop-OwnedProcess $secondProbe
    Stop-OwnedProcess $nodeProxy
    if ($null -ne $goFixture) {
        if (-not $goFixture.Process.WaitForExit(10000)) {
            Stop-OwnedProcess $goFixture
        } else {
            Save-ProcessLogs $goFixture
        }
    }
    if ($null -ne $summary) {
        $summary | ConvertTo-Json -Depth 12 |
            Set-Content -LiteralPath (Join-Path $Evidence 'result.json') -Encoding utf8
    }
}

if ($null -eq $summary) { throw 'Progressive uploaded-MP4 test did not produce a result.' }
$summary | ConvertTo-Json -Depth 12
