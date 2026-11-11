#Requires -Version 7.2
<#
.SYNOPSIS
    Back up or restore only the state touched by the native playback-scheduler probe.

.DESCRIPTION
    A live scheduler probe changes Glasswave's protected "Now Playing" playlist, the SDK
    queue, playback mode and the scheduler checkpoint. The probe restores those values in
    process. This script provides a disk-level safety net after a graceful app stop:

    - config.sqlite is copied intact as a safety backup.
    - only the Now Playing .fplite and -props.sqlite files are restored.
    - only cfg_var.6B42D915-0F86-4AA7-9C35-8A2F3D1E7054 is restored in config.sqlite.
    - the whole config.sqlite is never copied back, so unrelated owner settings such as
      the Apple Music region are preserved.

    Files are ordinary Copy-Item copies. The utility does not re-read or hash copied playlist
    payloads; functional scheduler/checkpoint state is verified separately.
#>
[CmdletBinding(DefaultParameterSetName = 'Backup')]
param(
    [Parameter(Mandatory, ParameterSetName = 'Backup')]
    [switch]$Backup,

    [Parameter(Mandatory, ParameterSetName = 'Restore')]
    [switch]$Restore,

    [Parameter(Mandatory, ParameterSetName = 'Restore')]
    [string]$SourceBackupPath,

    [string]$ProfileRoot,
    [string]$BackupRoot,
    [switch]$AllowNonStandardPaths,
    [switch]$SkipProcessCheck
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Root = Split-Path -Parent $PSScriptRoot
$Instance = Join-Path $Root '.local/fb2k-dev'
$DevExe = Join-Path $Instance 'foobar2000.exe'
$TestRoot = Join-Path $Root '.local/tests'
$CheckpointKey = 'cfg_var.6B42D915-0F86-4AA7-9C35-8A2F3D1E7054'

if ([string]::IsNullOrWhiteSpace($ProfileRoot)) {
    $ProfileRoot = Join-Path $Instance 'profile'
}
if ([string]::IsNullOrWhiteSpace($BackupRoot)) {
    $BackupRoot = $TestRoot
}

function Get-NormalizedPath {
    param([Parameter(Mandatory)][string]$Path)
    return [IO.Path]::GetFullPath($Path).TrimEnd(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar)
}

function Test-PathWithin {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Parent
    )
    $FullPath = Get-NormalizedPath $Path
    $FullParent = Get-NormalizedPath $Parent
    return $FullPath.Equals($FullParent, [StringComparison]::OrdinalIgnoreCase) -or
        $FullPath.StartsWith(
            $FullParent + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase)
}

function Assert-NoDevProcess {
    if (-not (Test-Path -LiteralPath $DevExe -PathType Leaf)) {
        throw "Dev executable is missing: $DevExe"
    }
    $Expected = (Resolve-Path -LiteralPath $DevExe).Path
    $Processes = @(
        Get-Process -Name foobar2000 -ErrorAction SilentlyContinue |
            Where-Object {
                try { $_.Path -eq $Expected }
                catch { $false }
            }
    )
    if ($Processes.Count -ne 0) {
        throw 'The Glasswave dev instance is running. Stop it gracefully first.'
    }
}

function Get-NowPlayingPlaylist {
    param([Parameter(Mandatory)][string]$PlaylistsPath)
    $IndexPath = Join-Path $PlaylistsPath 'index.txt'
    if (-not (Test-Path -LiteralPath $IndexPath -PathType Leaf)) {
        throw "Playlist index is missing: $IndexPath"
    }
    $Guids = @(
        Get-Content -LiteralPath $IndexPath |
            ForEach-Object {
                if ($_ -match '^([0-9A-Fa-f-]{36}):Now Playing\s*$') {
                    $Matches[1].ToUpperInvariant()
                }
            }
    )
    if ($Guids.Count -ne 1) {
        throw "Expected exactly one Now Playing playlist, found $($Guids.Count)."
    }
    return $Guids[0]
}

function Get-NowPlayingFiles {
    param(
        [Parameter(Mandatory)][string]$PlaylistsPath,
        [Parameter(Mandatory)][string]$Guid
    )
    return @(
        Join-Path $PlaylistsPath "playlist-$Guid.fplite"
        Join-Path $PlaylistsPath "playlist-$Guid-props.sqlite"
    )
}

function Assert-NoSidecars {
    param([Parameter(Mandatory)][string]$BasePath)
    foreach ($Suffix in @('-wal', '-shm')) {
        $Sidecar = "$BasePath$Suffix"
        if (Test-Path -LiteralPath $Sidecar) {
            throw "SQLite sidecar still exists after shutdown: $Sidecar"
        }
    }
}

function Copy-StateFiles {
    param(
        [Parameter(Mandatory)][string]$Destination,
        [Parameter(Mandatory)][string[]]$PlaylistFiles,
        [Parameter(Mandatory)][string]$ConfigPath
    )
    # Copy-Item is authoritative for playlist payloads. Do not add content re-reads,
    # hashes or byte comparisons around these user-state files.
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    foreach ($File in $PlaylistFiles) {
        if (-not (Test-Path -LiteralPath $File -PathType Leaf)) {
            throw "Now Playing file is missing: $File"
        }
        Copy-Item -LiteralPath $File -Destination $Destination
    }
    Copy-Item -LiteralPath $ConfigPath -Destination $Destination
}

if ($SkipProcessCheck -and -not $AllowNonStandardPaths) {
    throw '-SkipProcessCheck is allowed only with -AllowNonStandardPaths.'
}
$ProfileRoot = Get-NormalizedPath $ProfileRoot
$BackupRoot = Get-NormalizedPath $BackupRoot
if ($AllowNonStandardPaths) {
    if (-not (Test-PathWithin -Path $ProfileRoot -Parent $TestRoot) -or
        -not (Test-PathWithin -Path $BackupRoot -Parent $TestRoot)) {
        throw 'Non-standard profile and backup paths must remain under .local/tests.'
    }
} else {
    $ExpectedProfile = Get-NormalizedPath (Join-Path $Instance 'profile')
    if ($ProfileRoot -ne $ExpectedProfile -or
        -not (Test-PathWithin -Path $BackupRoot -Parent $TestRoot)) {
        throw 'Production paths must use the repository dev profile and .local/tests.'
    }
}

$PlaylistsPath = Join-Path $ProfileRoot 'playlists-v2.0'
$ConfigPath = Join-Path $ProfileRoot 'config.sqlite'
$NowPlayingGuid = Get-NowPlayingPlaylist $PlaylistsPath
$PlaylistFiles = Get-NowPlayingFiles $PlaylistsPath $NowPlayingGuid
Assert-NoSidecars $ConfigPath
foreach ($File in $PlaylistFiles) {
    Assert-NoSidecars $File
}

if (-not $SkipProcessCheck) {
    Assert-NoDevProcess
}

$Python = (Get-Command python -CommandType Application -ErrorAction Stop |
    Select-Object -First 1).Source

if ($Backup) {
    $Stamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
    $Destination = Join-Path $BackupRoot "playback-probe-state-$Stamp"
    if (Test-Path -LiteralPath $Destination) {
        throw "Backup destination already exists: $Destination"
    }
    Copy-StateFiles -Destination $Destination -PlaylistFiles $PlaylistFiles -ConfigPath $ConfigPath
    $Manifest = [ordered]@{
        createdAtUtc = [DateTime]::UtcNow.ToString('o')
        nowPlayingGuid = $NowPlayingGuid
        checkpointKey = $CheckpointKey
        files = @(
            Get-ChildItem -LiteralPath $Destination -File |
                Sort-Object Name |
                ForEach-Object {
                    [ordered]@{
                        name = $_.Name
                        length = $_.Length
                        lastWriteTimeUtc = $_.LastWriteTimeUtc.ToString('o')
                    }
                }
        )
    }
    $Manifest |
        ConvertTo-Json -Depth 6 |
        Set-Content -LiteralPath (Join-Path $Destination 'manifest.json') -Encoding utf8NoBOM
    Write-Host "Backup: $Destination"
    Write-Output $Destination
} else {
    $Source = Get-NormalizedPath $SourceBackupPath
    if (-not (Test-PathWithin -Path $Source -Parent $TestRoot)) {
        throw 'Restore source must remain under .local/tests.'
    }
    $ManifestPath = Join-Path $Source 'manifest.json'
    if (-not (Test-Path -LiteralPath $ManifestPath -PathType Leaf)) {
        throw "Backup manifest is missing: $ManifestPath"
    }
    $Manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json -ErrorAction Stop
    if ($Manifest.nowPlayingGuid -ne $NowPlayingGuid -or $Manifest.checkpointKey -ne $CheckpointKey) {
        throw 'Backup identity does not match the current Now Playing playlist and checkpoint.'
    }
    $SourceConfig = Join-Path $Source 'config.sqlite'
    foreach ($File in $PlaylistFiles | ForEach-Object { Join-Path $Source (Split-Path -Leaf $_) }) {
        if (-not (Test-Path -LiteralPath $File -PathType Leaf)) {
            throw "Restore source file is missing: $File"
        }
    }
    if (-not (Test-Path -LiteralPath $SourceConfig -PathType Leaf)) {
        throw "Restore source config is missing: $SourceConfig"
    }

    $PythonValidation = @'
import json, sqlite3, sys
from pathlib import Path

KEY = "cfg_var.6B42D915-0F86-4AA7-9C35-8A2F3D1E7054"
source_path = Path(sys.argv[1])
connection = sqlite3.connect(source_path.as_uri() + "?mode=ro", uri=True)
try:
    row = connection.execute(
        "SELECT typeof(value), value FROM configStrings WHERE name=?", (KEY,)
    ).fetchone()
    if row is None or row[0] != "text":
        raise SystemExit("backup checkpoint is missing or not TEXT")
    document = json.loads(row[1])
    count = len(document["canonical"])
    if document.get("version") != 2:
        raise SystemExit("backup checkpoint must be v2; run the one-off upgrade first")
    if document.get("projection") != list(range(count)):
        raise SystemExit("backup checkpoint projection does not match physical order")
    print(count)
finally:
    connection.close()
'@
    $CanonicalCount = & $Python -c $PythonValidation $SourceConfig
    if ($LASTEXITCODE -ne 0) {
        throw 'Backup checkpoint validation failed.'
    }

    $PreRestoreStamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
    $PreRestore = Join-Path $BackupRoot "playback-probe-prerestore-$PreRestoreStamp"
    Copy-StateFiles -Destination $PreRestore -PlaylistFiles $PlaylistFiles -ConfigPath $ConfigPath

    $PythonRestore = @'
import json, sqlite3, sys
from pathlib import Path

KEY = "cfg_var.6B42D915-0F86-4AA7-9C35-8A2F3D1E7054"
source = sqlite3.connect(Path(sys.argv[1]).as_uri() + "?mode=ro", uri=True)
target = sqlite3.connect(Path(sys.argv[2]), timeout=5, isolation_level=None)
target.execute("PRAGMA busy_timeout=5000")
try:
    source_row = source.execute(
        "SELECT typeof(value), value FROM configStrings WHERE name=?", (KEY,)
    ).fetchone()
    if source_row is None or source_row[0] != "text":
        raise SystemExit("backup checkpoint is missing or not TEXT")
    value = source_row[1]
    document = json.loads(value)
    count = len(document["canonical"])
    if document.get("version") != 2 or document.get("projection") != list(range(count)):
        raise SystemExit("backup checkpoint is not current v2")

    columns = target.execute("PRAGMA table_info(configStrings)").fetchall()
    actual = [(row[1], row[2].upper(), row[3], row[5]) for row in columns]
    if actual != [("name", "TEXT", 1, 1), ("value", "TEXT", 0, 0)]:
        raise SystemExit("target configStrings schema is unexpected")
    before = target.execute(
        "SELECT name, value FROM configStrings WHERE name<>? ORDER BY name", (KEY,)
    ).fetchall()
    before_count = target.execute("SELECT COUNT(*) FROM configStrings").fetchone()[0]
    existed = target.execute(
        "SELECT COUNT(*) FROM configStrings WHERE name=?", (KEY,)
    ).fetchone()[0]
    target.execute("BEGIN IMMEDIATE")
    try:
        target.execute(
            "INSERT INTO configStrings(name, value) VALUES(?, ?) "
            "ON CONFLICT(name) DO UPDATE SET value=excluded.value",
            (KEY, value),
        )
        restored = target.execute(
            "SELECT value FROM configStrings WHERE name=?", (KEY,)
        ).fetchone()
        after = target.execute(
            "SELECT name, value FROM configStrings WHERE name<>? ORDER BY name", (KEY,)
        ).fetchall()
        after_count = target.execute("SELECT COUNT(*) FROM configStrings").fetchone()[0]
        if restored is None or restored[0] != value:
            raise RuntimeError("checkpoint read-back failed")
        if after != before or after_count != before_count:
            raise RuntimeError("unrelated configStrings rows changed")
        target.execute("COMMIT")
    except BaseException:
        target.execute("ROLLBACK")
        raise
    print(json.dumps({"canonicalCount": count, "targetExisted": bool(existed)}))
finally:
    target.close()
    source.close()
'@
    $RestoreResult = & $Python -c $PythonRestore $SourceConfig $ConfigPath
    if ($LASTEXITCODE -ne 0) {
        throw 'Targeted checkpoint restore failed.'
    }
    foreach ($TargetFile in $PlaylistFiles) {
        $SourceFile = Join-Path $Source (Split-Path -Leaf $TargetFile)
        # Restore the ordinary copy as-is; payload verification belongs to the functional
        # scheduler assertions, not this file-copy utility.
        Copy-Item -LiteralPath $SourceFile -Destination $TargetFile -Force
    }
    $Evidence = [ordered]@{
        restoredAtUtc = [DateTime]::UtcNow.ToString('o')
        sourceBackup = $Source
        preRestoreBackup = $PreRestore
        nowPlayingGuid = $NowPlayingGuid
        checkpointKey = $CheckpointKey
        canonicalCount = [int]$CanonicalCount
        restoredPlaylistFiles = $PlaylistFiles
        restoredCheckpointRowOnly = $true
        restoredWholeConfigSqlite = $false
    }
    $EvidencePath = Join-Path $TestRoot "playback-probe-restore-$PreRestoreStamp.json"
    $Evidence |
        ConvertTo-Json -Depth 6 |
        Set-Content -LiteralPath $EvidencePath -Encoding utf8NoBOM
    Write-Host "Restored targeted probe state from: $Source"
    Write-Host "Pre-restore safety copy: $PreRestore"
    Write-Host "Evidence: $EvidencePath"
}
