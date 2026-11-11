#Requires -Version 7.2
<#
.SYNOPSIS
    Run the seven-case native shared-audio-cache regression in the repository dev instance.

.DESCRIPTION
    Writes a one-shot marker for cache/audio_probe.cpp, optionally deploys the current
    Release build, waits for the sanitized JSON result, validates every case, and copies
    the evidence to .local/tests/audio-cache-result.json.

.EXAMPLE
    pwsh -File scripts/test-audio-cache.ps1 -Deploy

.EXAMPLE
    pwsh -File scripts/test-audio-cache.ps1 -FixturePath .local/tests/aac-48-progressive.m4a
#>
[CmdletBinding()]
param(
    [string]$FixturePath = '.local/tests/alac-24-192-progressive.m4a',

    [ValidateRange(10, 300)]
    [int]$TimeoutSeconds = 120,

    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',

    [switch]$Deploy,

    [switch]$KeepRunning
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Instance = Join-Path $Root '.local/fb2k-dev'
$Exe = Join-Path $Instance 'foobar2000.exe'
$ProbeDir = Join-Path $Instance 'profile/glasswave/cache-probe'
$RequestPath = Join-Path $ProbeDir 'audio-request.json'
$ClaimedPath = Join-Path $ProbeDir 'audio-request.claimed.json'
$ResultPath = Join-Path $ProbeDir 'audio-result.json'
$EvidenceDir = Join-Path $Root '.local/tests'
$EvidencePath = Join-Path $EvidenceDir 'audio-cache-result.json'

if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) {
    throw 'Dev instance missing; bootstrap is out of scope for this probe.'
}

if (-not ('GlasswaveAudioProbePath' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

public static class GlasswaveAudioProbePath {
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
$allowedCanonical = [GlasswaveAudioProbePath]::Resolve($allowedRoot, $true)
$fixtureCanonical = [GlasswaveAudioProbePath]::Resolve($candidate, $false)
$relative = [IO.Path]::GetRelativePath($allowedCanonical, $fixtureCanonical)
if ([IO.Path]::IsPathRooted($relative) -or
    $relative -eq '..' -or
    $relative.StartsWith('..' + [IO.Path]::DirectorySeparatorChar, [StringComparison]::Ordinal) -or
    $relative.StartsWith('..' + [IO.Path]::AltDirectorySeparatorChar, [StringComparison]::Ordinal)) {
    throw 'FixturePath must stay under .local/tests.'
}
$fixture = Get-Item -LiteralPath $fixtureCanonical
if ($fixture.Extension -notin '.m4a', '.flac', '.mp3' -or $fixture.Length -le 0 -or $fixture.Length -gt 8MB) {
    throw 'FixturePath must be a bounded ALAC/AAC/FLAC/MP3 fixture.'
}

$devProcesses = @(
    Get-Process -Name foobar2000 -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -eq (Resolve-Path -LiteralPath $Exe).Path }
)
if ($devProcesses.Count -gt 0 -and -not $Deploy) {
    throw 'The dev instance is already running; rerun with -Deploy so startup consumes the marker.'
}

New-Item -ItemType Directory -Force -Path $ProbeDir | Out-Null
New-Item -ItemType Directory -Force -Path $EvidenceDir | Out-Null
Remove-Item -LiteralPath $ResultPath, $ClaimedPath, $EvidencePath -Force -ErrorAction SilentlyContinue

$runId = [Guid]::NewGuid().ToString('N')
$request = [ordered]@{
    runId = $runId
    fixturePath = $fixtureCanonical
}
[IO.File]::WriteAllText(
    $RequestPath,
    ($request | ConvertTo-Json -Compress),
    [Text.UTF8Encoding]::new($false))

$startedByScript = $false
try {
    if ($Deploy) {
        & (Join-Path $Root 'scripts/run-dev.ps1') -Configuration $Configuration
        $startedByScript = $true
    } else {
        Start-Process -FilePath $Exe -WorkingDirectory $Instance
        $startedByScript = $true
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
        throw "Audio-cache probe did not finish within $TimeoutSeconds seconds."
    }
    Copy-Item -LiteralPath $ResultPath -Destination $EvidencePath -Force
    $rawResult = Get-Content -LiteralPath $EvidencePath -Raw
    if ($rawResult -match '(?i)https?://|x-api-token|sessionId') {
        throw 'Audio-cache evidence contains a URL or credential-shaped field.'
    }

    $required = @(
        'ok', 'runId', 'provider', 'fixtureName', 'fixtureBytes', 'codec', 'sampleRate',
        'bitDepth', 'channels', 'durationSeconds', 'keyInvariants', 'prefetchScope',
        'cleanupErrors', 'cases', 'error'
    )
    foreach ($name in $required) {
        if (-not $result.PSObject.Properties.Name.Contains($name)) {
            throw 'Audio-cache result is incomplete.'
        }
    }
    if ($result.runId -cne $runId -or $result.provider -cne 'glasswave-cache-test') {
        throw 'Audio-cache result identity mismatch.'
    }
    if (-not $result.keyInvariants -or -not $result.prefetchScope -or [int]$result.cleanupErrors -ne 0) {
        throw 'Audio-cache global invariants failed.'
    }

    $names = @(
        'progressive_first_decode',
        'cache_hit',
        'concurrent_readers',
        'concurrent_reader_io',
        'invalidate_during_produce',
        'scope_rotation',
        'staging_last_release'
    )
    if (@($result.cases).Count -ne $names.Count) {
        throw 'Audio-cache result must contain exactly seven cases.'
    }
    for ($i = 0; $i -lt $names.Count; ++$i) {
        if ($result.cases[$i].name -cne $names[$i] -or -not $result.cases[$i].ok) {
            throw "Audio-cache case failed: $($names[$i]) ($($result.cases[$i].error))"
        }
    }

    $progressive = $result.cases[0]
    $hit = $result.cases[1]
    $concurrent = $result.cases[2]
    $parallel = $result.cases[3]
    $invalidation = $result.cases[4]
    $scope = $result.cases[5]
    $staging = $result.cases[6]
    if (-not $progressive.progressive -or [double]$progressive.firstDecodedMs -lt 0 -or
        [int]$progressive.producerCalls -ne 1 -or -not $progressive.bytesMatch -or
        $progressive.progressiveSizeKnown -or $progressive.progressiveCanSeek) {
        throw 'Progressive first-decode assertions failed.'
    }
    if (-not $hit.cacheHit -or [int]$hit.fetcherCalls -ne 0 -or -not $hit.bytesMatch) {
        throw 'Cache-hit assertions failed.'
    }
    if ([int]$concurrent.producerCalls -ne 1 -or [int]$concurrent.fetcherCalls -ne 0 -or
        -not $concurrent.positionsIndependent -or -not $concurrent.cancelIsolated) {
        throw 'Concurrent-reader assertions failed.'
    }
    if ([int]$parallel.parallelReaders -ne 8 -or [int]$parallel.parallelPasses -ne 6 -or
        [uint64]$parallel.bytesVerified -lt 96MB -or -not $parallel.nonpersistentFallback -or
        $parallel.progressiveSizeKnown -or $parallel.progressiveCanSeek -or
        -not $parallel.completedSizeKnown -or -not $parallel.completedCanSeek) {
        throw 'Parallel reader I/O assertions failed.'
    }
    if ($invalidation.publishVisible -or [int]$invalidation.activeFlights -ne 0) {
        throw 'Invalidation published or left an active flight.'
    }
    if ($scope.publishVisible -or [int]$scope.activeFlights -ne 0 -or -not $scope.bytesMatch) {
        throw 'Scope-rotation assertions failed.'
    }
    if ([int]$staging.stagingDuring -le 0 -or [int]$staging.stagingAfter -ne 0 -or
        [int]$staging.activeFlights -ne 0) {
        throw 'Staging-cleanup assertions failed.'
    }
    if (-not $result.ok) {
        $message = if ([string]::IsNullOrWhiteSpace([string]$result.error)) {
            'unspecified failure'
        } else {
            [string]$result.error
        }
        throw "Audio-cache probe failed: $message"
    }

    $result.cases |
        Select-Object name,
            @{ Name = 'producer'; Expression = { [int]$_.producerCalls } },
            @{ Name = 'fetcher'; Expression = { [int]$_.fetcherCalls } },
            @{ Name = 'readers'; Expression = { [int]$_.parallelReaders } },
            @{ Name = 'verifiedMB'; Expression = { [math]::Round([double]$_.bytesVerified / 1MB, 1) } },
            @{ Name = 'firstMs'; Expression = { $_.firstDecodedMs } },
            @{ Name = 'active'; Expression = { [int]$_.activeFlights } } |
        Format-Table -AutoSize
    Write-Host "Audio-cache evidence: $EvidencePath"
} finally {
    if ($startedByScript -and -not $KeepRunning) {
        Get-Process -Name foobar2000 -ErrorAction SilentlyContinue |
            Where-Object { $_.Path -eq (Resolve-Path -LiteralPath $Exe).Path } |
            Stop-Process -Force
    }
}
