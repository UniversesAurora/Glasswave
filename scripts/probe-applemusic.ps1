#Requires -Version 7.2
<#
.SYNOPSIS
    Run the one-shot Apple Music decoder probe in the repository's dev instance.

.DESCRIPTION
    Writes one request into .local/fb2k-dev/profile/glasswave/applemusic, optionally
    restarts that instance through scripts/run-dev.ps1, and waits for the decoder
    result. Catalog input is selected by the Apple helper; local fixtures exercise the
    same foobar2000 input and decoder path without an account.

.EXAMPLE
    pwsh -File scripts/probe-applemusic.ps1 -SongId 123456789 -Deploy

.EXAMPLE
    pwsh -File scripts/probe-applemusic.ps1 -FixturePath .local/tests/alac-24-192.m4a -Deploy
#>
[CmdletBinding(DefaultParameterSetName = 'Song')]
param(
    [Parameter(Mandatory, ParameterSetName = 'Song')]
    [ValidateNotNullOrEmpty()]
    [string]$SongId,

    [Parameter(Mandatory, ParameterSetName = 'Fixture')]
    [ValidateNotNullOrEmpty()]
    [string]$FixturePath,

    [ValidateRange(1, 600)]
    [int]$TimeoutSeconds = 120,

    [switch]$VerifyMetadata,

    [switch]$FixtureMapping,

    [switch]$CancelStreamTest,

    [switch]$SeekStress,

    [switch]$Atmos,

    [ValidateSet('StereoSpeakers', 'Headphones', 'Speakers51', 'Speakers71')]
    [string]$AtmosOutput = 'StereoSpeakers',

    [switch]$Deploy
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Instance = Join-Path $Root '.local/fb2k-dev'
$Exe = Join-Path $Instance 'foobar2000.exe'
$ProbeDir = Join-Path $Instance 'profile/glasswave/applemusic'
$RequestPath = Join-Path $ProbeDir 'probe-request.json'
$ResultPath = Join-Path $ProbeDir 'probe-result.json'
$BridgePath = Join-Path $ProbeDir 'bridge.json'

if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) {
    throw 'Dev instance missing; bootstrap is out of scope for this probe.'
}

if ($PSCmdlet.ParameterSetName -eq 'Song') {
    if ($SongId.Length -gt 1024 -or $SongId -match '[\x00-\x1F\x7F]') {
        throw 'SongId is invalid.'
    }
    if ($PSBoundParameters.ContainsKey('AtmosOutput') -and -not $Atmos) {
        throw '-AtmosOutput requires -Atmos.'
    }
    $request = [ordered]@{ songId = $SongId }
    if ($FixtureMapping -and ($SongId -cne 'glasswave-test' -or -not $VerifyMetadata -or
        $CancelStreamTest -or $Atmos)) {
        throw '-FixtureMapping requires the glasswave-test metadata fixture without cancellation or Atmos.'
    }
    if ($VerifyMetadata -or $CancelStreamTest) {
        if ($CancelStreamTest -and $SongId -cne 'glasswave-test') {
            throw '-CancelStreamTest only permits the glasswave-test fixture asset ID.'
        }
        if (-not (Test-Path -LiteralPath $BridgePath -PathType Leaf)) {
            throw 'Fixture verification requires the local Apple Music fixture bridge.'
        }
        try {
            $bridge = Get-Content -LiteralPath $BridgePath -Raw | ConvertFrom-Json -ErrorAction Stop
        } catch {
            throw 'Fixture verification could not validate the local Apple Music fixture bridge.'
        }
        $fixtureMappingMode = if ([string]::IsNullOrWhiteSpace([string]$bridge.mappingMode)) {
            'Normal'
        } else {
            [string]$bridge.mappingMode
        }
        $expectedPlaybackAsset = if ($fixtureMappingMode -eq 'Normal') {
            'glasswave-test'
        } else {
            '1237064886'
        }
        if ([string]$bridge.fixtureAssetId -cne 'glasswave-test' -or
            [string]$bridge.fixtureTransactionTag -cnotmatch '^[0-9a-f]{32}$' -or
            [string]$bridge.fixturePlaybackAssetId -cne $expectedPlaybackAsset -or
            $SongId -cne 'glasswave-test' -or
            [bool]$FixtureMapping -ne ($fixtureMappingMode -ne 'Normal') -or
            ($FixtureMapping -and ([string]$bridge.libraryId -cne 'i.GlasswaveStaleFixture' -or
                [string]$bridge.reportingId -cne '1865440142'))) {
            throw 'Fixture verification refuses a non-fixture Apple Music bridge.'
        }
    }
    if ($VerifyMetadata) {
        $request.verifyMetadata = $true
    }
    if ($FixtureMapping) {
        $request.fixtureMapping = $true
    }
    if ($CancelStreamTest) {
        $request.cancelAfterFirstChunk = $true
    }
    if ($Atmos) {
        $request.atmos = $true
        $request.atmosOutput = $AtmosOutput
    }
} else {
    if ($VerifyMetadata -or $FixtureMapping -or $CancelStreamTest -or $Atmos) {
        throw 'Fixture verification options are forbidden in local file mode.'
    }
    if (-not ('GlasswaveProbePath' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

public static class GlasswaveProbePath {
    private const uint FileReadAttributes = 0x00000080;
    private const uint FileShareReadWriteDelete = 0x00000007;
    private const uint OpenExisting = 3;
    private const uint FileFlagBackupSemantics = 0x02000000;
    private const uint FileNameNormalized = 0;
    private const uint VolumeNameDos = 0;

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern SafeFileHandle CreateFileW(
        string fileName,
        uint desiredAccess,
        uint shareMode,
        IntPtr securityAttributes,
        uint creationDisposition,
        uint flagsAndAttributes,
        IntPtr templateFile);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern uint GetFinalPathNameByHandleW(
        SafeFileHandle file,
        StringBuilder path,
        uint pathLength,
        uint flags);

    public static string Resolve(string path, bool directory) {
        uint flags = directory ? FileFlagBackupSemantics : 0;
        using (SafeFileHandle handle = CreateFileW(
            path,
            FileReadAttributes,
            FileShareReadWriteDelete,
            IntPtr.Zero,
            OpenExisting,
            flags,
            IntPtr.Zero)) {
            if (handle.IsInvalid) {
                throw new Win32Exception(Marshal.GetLastWin32Error());
            }
            var buffer = new StringBuilder(32768);
            uint length = GetFinalPathNameByHandleW(
                handle,
                buffer,
                (uint)buffer.Capacity,
                FileNameNormalized | VolumeNameDos);
            if (length == 0 || length >= buffer.Capacity) {
                throw new Win32Exception(Marshal.GetLastWin32Error());
            }
            string resolved = buffer.ToString();
            if (resolved.StartsWith(@"\\?\UNC\", StringComparison.OrdinalIgnoreCase)) {
                resolved = @"\\" + resolved.Substring(8);
            } else if (resolved.StartsWith(@"\\?\", StringComparison.Ordinal)) {
                resolved = resolved.Substring(4);
            }
            return System.IO.Path.GetFullPath(resolved);
        }
    }
}
'@
    }

    $candidate = if ([IO.Path]::IsPathRooted($FixturePath)) {
        $FixturePath
    } else {
        Join-Path $Root $FixturePath
    }
    if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
        throw 'FixturePath must name an existing file.'
    }
    $allowedRoot = Join-Path $Root '.local/tests'
    if (-not (Test-Path -LiteralPath $allowedRoot -PathType Container)) {
        throw 'The repository test-fixture directory is missing.'
    }
    $allowedCanonical = [GlasswaveProbePath]::Resolve($allowedRoot, $true)
    $fixtureCanonical = [GlasswaveProbePath]::Resolve($candidate, $false)
    $relative = [IO.Path]::GetRelativePath($allowedCanonical, $fixtureCanonical)
    if ([IO.Path]::IsPathRooted($relative) -or
        $relative -eq '..' -or
        $relative.StartsWith('..' + [IO.Path]::DirectorySeparatorChar, [StringComparison]::Ordinal) -or
        $relative.StartsWith('..' + [IO.Path]::AltDirectorySeparatorChar, [StringComparison]::Ordinal)) {
        throw 'FixturePath must stay under .local/tests.'
    }
    $request = [ordered]@{ localFixture = $fixtureCanonical }
}

$devProcesses = @(
    Get-Process -Name foobar2000 -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -eq (Resolve-Path -LiteralPath $Exe).Path }
)
if ($devProcesses.Count -gt 0 -and -not $Deploy) {
    throw 'The dev instance is already running; rerun with -Deploy so its startup probe can consume the request.'
}

New-Item -ItemType Directory -Force -Path $ProbeDir | Out-Null
if ($SeekStress) { $request.seekStress = $true }
Remove-Item -LiteralPath $ResultPath -Force -ErrorAction SilentlyContinue
$json = $request | ConvertTo-Json -Compress
[IO.File]::WriteAllText($RequestPath, $json, [Text.UTF8Encoding]::new($false))

if ($Deploy) {
    $runDev = Join-Path $Root 'scripts/run-dev.ps1'
    & $runDev -Configuration Release
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
    throw "Decoder probe did not finish within $TimeoutSeconds seconds."
}

$required = @(
    'ok', 'codec', 'bitDepth', 'sampleRate', 'channels', 'samples',
    'decodedSeconds', 'declaredSeconds', 'canSeek', 'seekVerified',
    'metadataVerified', 'canceledAfterFirstChunk', 'shutdownMs',
    'metadataIdentityVerified', 'metadataInputPath', 'metadataSource', 'metadataDisplay',
    'metadataGetInfo', 'metadataDynamicInfo', 'decoderError',
    'fixtureTransactionRestored',
    'firstDecodedMs', 'firstDecodedUnixMs',
    'atmosRendered', 'atmosObjects', 'renderOutput', 'channelMask', 'error'
)
foreach ($name in $required) {
    if (-not $result.PSObject.Properties.Name.Contains($name)) {
        throw 'Decoder result is incomplete.'
    }
}

$codec = if ($null -eq $result.codec) { 'unknown' } else { [string]$result.codec }
$bitDepth = if ($null -eq $result.bitDepth) { 'unknown' } else { "$($result.bitDepth) bit" }
$sampleRate = if ($null -eq $result.sampleRate) { 'unknown' } else { "$($result.sampleRate) Hz" }
$channels = if ($null -eq $result.channels) { 'unknown' } else { [string]$result.channels }
$samples = if ($null -eq $result.samples) { 'unknown' } else { [string]$result.samples }
$decoded = if ($null -eq $result.decodedSeconds) { 'unknown' } else { '{0:F6} s' -f [double]$result.decodedSeconds }
$declared = if ($null -eq $result.declaredSeconds) { 'unknown' } else { '{0:F6} s' -f [double]$result.declaredSeconds }
$seek = if ($result.seekVerified) { 'verified' } elseif ($result.canSeek) { 'reported but unverified' } else { 'unavailable' }
$firstDecoded = if ($null -eq $result.firstDecodedMs) { 'not decoded' } else { '{0:F3} ms' -f [double]$result.firstDecodedMs }

Write-Host "Format: $codec"
Write-Host "Bit depth: $bitDepth"
Write-Host "Sample rate: $sampleRate"
Write-Host "Channels: $channels"
Write-Host "Samples: $samples"
Write-Host "Decoded: $decoded"
Write-Host "Declared: $declared"
Write-Host "Seek: $seek"
if ($SeekStress) {
    Write-Host "Early seek transitions: $($result.seekStressCount)"
    if ($result.seekStressCount -ne 6) { throw 'Early seek regression did not complete all transitions.' }
}
Write-Host "First decoded: $firstDecoded"
if ($VerifyMetadata) {
    $metadataStatus = if ($result.metadataVerified -and $result.metadataIdentityVerified) { 'verified' } else { 'unverified' }
    Write-Host "Metadata tags: $metadataStatus"
}
if ($CancelStreamTest) {
    $shutdown = if ($null -eq $result.shutdownMs) { 'unknown' } else { '{0:F3} ms' -f [double]$result.shutdownMs }
    Write-Host "Shutdown after first chunk: $shutdown"
}
if ($Atmos) {
    $objects = if ($null -eq $result.atmosObjects) { 'unknown' } else { [string]$result.atmosObjects }
    $output = if ($null -eq $result.renderOutput) { 'unknown' } else { [string]$result.renderOutput }
    $mask = if ($null -eq $result.channelMask) { 'unknown' } else { '0x{0:X}' -f [uint32]$result.channelMask }
    Write-Host "Atmos objects: $objects"
    Write-Host "Atmos output: $output (mask $mask)"
}
if (-not $result.ok) {
    $message = if ([string]::IsNullOrWhiteSpace([string]$result.error)) { 'unspecified failure' } else { [string]$result.error }
    Write-Host "Error: $message"
    if (-not [string]::IsNullOrWhiteSpace([string]$result.decoderError)) {
        Write-Host "Decoder error: $($result.decoderError)"
    }
    exit 1
}
if ($VerifyMetadata -and (-not $result.metadataVerified -or -not $result.metadataIdentityVerified -or
    -not $result.fixtureTransactionRestored)) {
    throw 'Catalog title/artist metadata was not retained by get_info and first get_dynamic_info.'
}
if ($CancelStreamTest) {
    if (-not $result.canceledAfterFirstChunk) {
        throw 'The probe did not cancel immediately after the first decoded chunk.'
    }
    if ($null -eq $result.shutdownMs -or [double]$result.shutdownMs -ge 2000) {
        throw "Decoder shutdown exceeded 2000 ms: $($result.shutdownMs)"
    }
}
if ($Atmos) {
    $codecText = [string]$result.codec
    if ($codecText -notmatch '(?i)E-AC-3|EAC3' -or $codecText -notmatch '(?i)JOC') {
        throw "Atmos source must be E-AC-3 with JOC; got '$codecText'."
    }
    if (-not $result.atmosRendered -or [int]$result.atmosObjects -le 0) {
        throw 'Cavern did not report rendered JOC objects.'
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
        [uint32]$result.channelMask -ne [uint32]$expectedMask) {
        throw "Atmos renderer output mismatch: channels=$($result.channels), mask=$($result.channelMask)."
    }
    if ([string]$result.renderOutput -cne $expectedOutput) {
        throw "Atmos output mode mismatch: got '$($result.renderOutput)', expected '$expectedOutput'."
    }
}
