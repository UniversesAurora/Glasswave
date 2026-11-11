#Requires -Version 7.2
<#
.SYNOPSIS
    Upgrade or safely discard Glasswave's playback scheduler checkpoint.

.DESCRIPTION
    Converts the single v1 scheduler checkpoint to the current v2 schema by preserving
    every stored role array and adding projection=[0,1,...,N-1]. The conversion is valid
    because v1 stores all role values as indices into the current physical playlist.

    The script uses one configStrings row, one SQLite transaction and a compare-and-swap
    update. After shutdown it makes an ordinary Copy-Item backup of config.sqlite before
    the first write. If the v1 JSON is invalid, only the scheduler checkpoint key is
    deleted. Unknown schemas stop without modifying the database.

    Execution is opt-in. Without -Execute the script performs read-only validation and
    reports the action it would take. With -Execute it refuses to run while the repository
    dev instance is active.
#>
[CmdletBinding()]
param(
    [string]$DatabasePath,
    [string]$PlaylistPath,
    [string]$BackupDirectory,
    [string]$EvidencePath,
    [switch]$Execute,
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

if ([string]::IsNullOrWhiteSpace($DatabasePath)) {
    $DatabasePath = Join-Path $Instance 'profile/config.sqlite'
}
if ([string]::IsNullOrWhiteSpace($PlaylistPath)) {
    $PlaylistPath = Join-Path $Instance 'profile/playlists-v2.0'
}
if ([string]::IsNullOrWhiteSpace($BackupDirectory)) {
    $BackupDirectory = $TestRoot
}
if ([string]::IsNullOrWhiteSpace($EvidencePath)) {
    $EvidencePath = Join-Path $TestRoot 'playback-checkpoint-v2-upgrade.json'
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
        throw "The Glasswave dev instance is running. Stop it before checkpoint migration."
    }
}

function Assert-NoDatabaseSidecars {
    foreach ($Suffix in @('-wal', '-shm')) {
        $Sidecar = "$DatabasePath$Suffix"
        if (Test-Path -LiteralPath $Sidecar) {
            throw "SQLite sidecar still exists after shutdown: $Sidecar"
        }
    }
}

function Get-PlaylistSnapshot {
    param([Parameter(Mandatory)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "Playlist storage directory is missing: $Path"
    }
    $FullPath = Get-NormalizedPath $Path
    $RootState = '|True||{0}' -f (Get-Item -LiteralPath $FullPath).LastWriteTimeUtc.Ticks
    return @($RootState) + @(
        Get-ChildItem -LiteralPath $FullPath -Recurse -Force |
            Sort-Object FullName |
            ForEach-Object {
                $Relative = $_.FullName.Substring($FullPath.Length).TrimStart('\', '/')
                '{0}|{1}|{2}|{3}' -f $Relative, $_.PSIsContainer, $_.Length,
                    $_.LastWriteTimeUtc.Ticks
            }
    ) -join "`n"
}

$DatabasePath = Get-NormalizedPath $DatabasePath
$PlaylistPath = Get-NormalizedPath $PlaylistPath
$BackupDirectory = Get-NormalizedPath $BackupDirectory
$EvidencePath = Get-NormalizedPath $EvidencePath
$ProfilePath = Get-NormalizedPath (Join-Path $Instance 'profile')

if ($SkipProcessCheck -and -not $AllowNonStandardPaths) {
    throw '-SkipProcessCheck is allowed only with -AllowNonStandardPaths.'
}
if ($AllowNonStandardPaths) {
    if (-not (Test-PathWithin -Path $DatabasePath -Parent $TestRoot) -or
        -not (Test-PathWithin -Path $PlaylistPath -Parent $TestRoot) -or
        -not (Test-PathWithin -Path $BackupDirectory -Parent $TestRoot) -or
        -not (Test-PathWithin -Path $EvidencePath -Parent $TestRoot)) {
        throw 'Non-standard database, playlist, backup and evidence paths must remain under .local/tests.'
    }
} else {
    $ExpectedDatabase = Get-NormalizedPath (Join-Path $ProfilePath 'config.sqlite')
    $ExpectedPlaylists = Get-NormalizedPath (Join-Path $ProfilePath 'playlists-v2.0')
    if ($DatabasePath -ne $ExpectedDatabase -or $PlaylistPath -ne $ExpectedPlaylists) {
        throw 'Production database and playlist paths do not match the repository dev profile.'
    }
    if (-not (Test-PathWithin -Path $DatabasePath -Parent $ProfilePath) -or
        -not (Test-PathWithin -Path $PlaylistPath -Parent $ProfilePath) -or
        -not (Test-PathWithin -Path $EvidencePath -Parent $TestRoot) -or
        -not (Test-PathWithin -Path $BackupDirectory -Parent $TestRoot)) {
        throw 'Production paths must use the repository dev profile and .local/tests.'
    }
}
if (Test-PathWithin -Path $DatabasePath -Parent $PlaylistPath) {
    throw 'config.sqlite must not reside inside the playlist storage tree.'
}
if (Test-PathWithin -Path $BackupDirectory -Parent $PlaylistPath) {
    throw 'Backup output must not reside inside the playlist storage tree.'
}
if (Test-PathWithin -Path $EvidencePath -Parent $PlaylistPath) {
    throw 'Evidence output must not reside inside the playlist storage tree.'
}

if (-not (Test-Path -LiteralPath $DatabasePath -PathType Leaf)) {
    throw "config.sqlite is missing: $DatabasePath"
}
if ($Execute -and -not $SkipProcessCheck) {
    Assert-NoDevProcess
}

$Python = (Get-Command python -CommandType Application -ErrorAction Stop |
    Select-Object -First 1).Source
$PythonVersion = & $Python -c 'import sys,sqlite3; print(sys.version.split()[0], sqlite3.sqlite_version)'
if ($LASTEXITCODE -ne 0) {
    throw 'Python with sqlite3 support is required for checkpoint migration.'
}

$PlaylistBefore = Get-PlaylistSnapshot $PlaylistPath
$Stamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
$BackupPath = Join-Path $BackupDirectory "config-before-playback-checkpoint-v2-$Stamp.sqlite"
if ($Execute) {
    if (-not $SkipProcessCheck) {
        Assert-NoDevProcess
    }
    New-Item -ItemType Directory -Force -Path $BackupDirectory | Out-Null
    if (Test-Path -LiteralPath $BackupPath) {
        throw "Backup path already exists: $BackupPath"
    }
    Assert-NoDatabaseSidecars
    Copy-Item -LiteralPath $DatabasePath -Destination $BackupPath
}

$PythonCode = @'
import argparse
import copy
import json
import re
import sqlite3
import sys
from pathlib import Path

KEY = "cfg_var.6B42D915-0F86-4AA7-9C35-8A2F3D1E7054"
MAX_CHECKPOINT_BYTES = 8 * 1024 * 1024
V1_KEYS = {
    "version", "shuffle", "repeat", "fingerprint", "current", "canonical",
    "history", "future", "priority", "prioritySizes", "pending",
}
V2_KEYS = V1_KEYS | {"projection"}


def fail(message):
    print(f"ERROR: {message}", file=sys.stderr)
    raise SystemExit(2)


def object_without_duplicate_keys(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def parse_json(text):
    return json.loads(text, object_pairs_hook=object_without_duplicate_keys)


def is_int(value):
    return type(value) is int


def validate_checkpoint(value, expected_version):
    if not isinstance(value, str):
        fail("checkpoint value is not TEXT")
    if not value or len(value.encode("utf-8")) > MAX_CHECKPOINT_BYTES:
        fail("checkpoint size is outside the accepted bound")
    try:
        document = parse_json(value)
    except Exception as error:
        raise ValueError(f"invalid checkpoint JSON: {error}") from error
    if not isinstance(document, dict):
        raise ValueError("checkpoint JSON must be an object")
    expected_keys = V1_KEYS if expected_version == 1 else V2_KEYS
    if set(document) != expected_keys:
        raise ValueError(
            f"checkpoint keys do not match v{expected_version}: "
            f"{sorted(set(document) ^ expected_keys)}")
    if document["version"] != expected_version or not is_int(document["version"]):
        raise ValueError("checkpoint version does not match")
    if type(document["shuffle"]) is not bool:
        raise ValueError("shuffle must be boolean")
    if not is_int(document["repeat"]) or document["repeat"] not in (0, 1, 2):
        raise ValueError("repeat must be 0, 1 or 2")

    canonical = document["canonical"]
    if not isinstance(canonical, list) or not all(is_int(index) for index in canonical):
        raise ValueError("canonical must be an integer array")
    count = len(canonical)
    if sorted(canonical) != list(range(count)):
        raise ValueError("canonical is not a permutation of physical indices")

    for name in ("history", "future", "priority", "prioritySizes", "pending"):
        values = document[name]
        if not isinstance(values, list) or not all(is_int(index) for index in values):
            raise ValueError(f"{name} must be an integer array")
    for name in ("history", "future", "priority", "pending"):
        if any(index < 0 or index >= count for index in document[name]):
            raise ValueError(f"{name} contains an out-of-range physical index")

    future = document["future"]
    priority = document["priority"]
    pending = document["pending"]
    if len(future) != len(set(future)):
        raise ValueError("future contains duplicate indices")
    if len(priority) != len(set(priority)):
        raise ValueError("priority contains duplicate indices")
    if len(set(priority + future)) != len(priority) + len(future):
        raise ValueError("priority and future overlap")
    if len(pending) != len(set(pending)):
        raise ValueError("pending contains duplicate indices")
    if set(pending) & set(priority + future):
        raise ValueError("pending overlaps future roles")

    priority_sizes = document["prioritySizes"]
    if any(size <= 0 for size in priority_sizes) or sum(priority_sizes) != len(priority):
        raise ValueError("prioritySizes does not describe the priority array")

    current = document["current"]
    if not is_int(current) or current < -1 or current >= count:
        raise ValueError("current is out of range")
    if current >= 0 and current in set(priority + future):
        raise ValueError("current overlaps a future role")

    fingerprint = document["fingerprint"]
    if not isinstance(fingerprint, str) or not re.fullmatch(r"[0-9A-Fa-f]*", fingerprint):
        raise ValueError("fingerprint is not hexadecimal text")

    if expected_version == 2:
        projection = document["projection"]
        if projection != list(range(count)):
            raise ValueError("projection is not the current physical order")
    return document


def checkpoint_row(connection):
    rows = connection.execute(
        "SELECT name, typeof(value), value FROM configStrings WHERE name = ?",
        (KEY,),
    ).fetchall()
    if len(rows) > 1:
        fail("checkpoint key is not unique")
    return rows[0] if rows else None


def verify_schema(connection):
    tables = connection.execute(
        "SELECT name FROM sqlite_master WHERE type='table' AND name='configStrings'"
    ).fetchall()
    if tables != [("configStrings",)]:
        fail("configStrings table is missing or ambiguous")
    columns = connection.execute("PRAGMA table_info(configStrings)").fetchall()
    actual = [(row[1], row[2].upper(), row[3], row[5]) for row in columns]
    expected = [("name", "TEXT", 1, 1), ("value", "TEXT", 0, 0)]
    if actual != expected:
        fail(f"unexpected configStrings schema: {actual}")


def verify_backup_database(backup_path):
    check = sqlite3.connect(Path(backup_path).as_uri() + "?mode=ro", uri=True)
    try:
        verify_schema(check)
        return checkpoint_row(check)
    finally:
        check.close()


ns = argparse.Namespace()
parser = argparse.ArgumentParser()
parser.add_argument("--database", required=True)
parser.add_argument("--execute", action="store_true")
parser.add_argument("--backup-path")
ns = parser.parse_args()

database = Path(ns.database).resolve()
connection = sqlite3.connect(database, timeout=5, isolation_level=None)
connection.execute("PRAGMA busy_timeout=5000")
try:
    verify_schema(connection)
    row = checkpoint_row(connection)
    total_rows = connection.execute("SELECT COUNT(*) FROM configStrings").fetchone()[0]
    if row is None:
        result = {
            "action": "noop-missing-checkpoint",
            "execute": ns.execute,
            "backupPath": None,
        }
    else:
        name, value_type, value = row
        if value_type != "text":
            fail(f"checkpoint SQLite storage class is {value_type}, expected text")
        try:
            parsed = parse_json(value)
            version = parsed.get("version") if isinstance(parsed, dict) else None
        except Exception:
            parsed = None
            version = 1 if re.search(r'"version"\s*:\s*1(?:\D|$)', value) else None

        if version == 2:
            try:
                document = validate_checkpoint(value, 2)
            except ValueError as error:
                fail(f"existing v2 checkpoint is invalid: {error}")
            result = {
                "action": "noop-current-v2",
                "execute": ns.execute,
                "versionBefore": 2,
                "versionAfter": 2,
                "canonicalCount": len(document["canonical"]),
                "current": document["current"],
                "priorityCount": len(document["priority"]),
                "checkpointBytes": len(value.encode("utf-8")),
                "backupPath": None,
            }
        elif version == 1:
            try:
                document = validate_checkpoint(value, 1)
                action = "upgrade-v1"
            except ValueError:
                action = "delete-invalid-v1"

            if action == "upgrade-v1":
                upgraded = copy.deepcopy(document)
                upgraded["version"] = 2
                upgraded["projection"] = list(range(len(upgraded["canonical"])))
                replacement = json.dumps(upgraded, ensure_ascii=False, separators=(",", ":"))
                if len(replacement.encode("utf-8")) > MAX_CHECKPOINT_BYTES:
                    fail("upgraded checkpoint exceeds the accepted bound")
                for key in V1_KEYS - {"version"}:
                    if upgraded[key] != document[key]:
                        fail(f"upgrade unexpectedly changed {key}")
            else:
                replacement = None

            if ns.execute:
                if not ns.backup_path:
                    fail("--backup-path is required for a write")
                backup_row = verify_backup_database(ns.backup_path)
                if backup_row != row:
                    fail("checkpoint backup verification failed")
                connection.execute("BEGIN IMMEDIATE")
                try:
                    current_row = checkpoint_row(connection)
                    if current_row != row:
                        fail("checkpoint changed after backup")
                    if action == "upgrade-v1":
                        changed = connection.execute(
                            "UPDATE configStrings SET value = ? WHERE name = ? AND value = ?",
                            (replacement, KEY, value),
                        ).rowcount
                        if changed != 1:
                            fail("compare-and-swap checkpoint update failed")
                        written = checkpoint_row(connection)
                        if written is None or written[2] != replacement:
                            fail("checkpoint read-back failed")
                        validate_checkpoint(written[2], 2)
                        after_rows = connection.execute(
                            "SELECT COUNT(*) FROM configStrings").fetchone()[0]
                        if after_rows != total_rows:
                            fail("unrelated configStrings rows changed")
                    else:
                        changed = connection.execute(
                            "DELETE FROM configStrings WHERE name = ? AND value = ?",
                            (KEY, value),
                        ).rowcount
                        if changed != 1:
                            fail("compare-and-swap checkpoint delete failed")
                        if checkpoint_row(connection) is not None:
                            fail("checkpoint row still exists after delete")
                        after_rows = connection.execute(
                            "SELECT COUNT(*) FROM configStrings").fetchone()[0]
                        if after_rows != total_rows - 1:
                            fail("unrelated configStrings rows changed")
                    connection.execute("COMMIT")
                except BaseException:
                    connection.execute("ROLLBACK")
                    raise
            result = {
                "action": action,
                "execute": ns.execute,
                "versionBefore": 1,
                "versionAfter": 2 if action == "upgrade-v1" else None,
                "canonicalCount": len(document["canonical"]) if action == "upgrade-v1" else None,
                "current": document.get("current") if action == "upgrade-v1" else None,
                "priorityCount": len(document.get("priority", [])) if action == "upgrade-v1" else None,
                "checkpointBytes": len(value.encode("utf-8")),
                "backupPath": ns.backup_path if ns.execute else None,
            }
        else:
            fail("checkpoint version is neither v1 nor v2; refusing to delete unknown data")
    print(json.dumps(result, separators=(",", ":")))
finally:
    connection.close()
'@

$PythonArguments = @('-c', $PythonCode, '--database', $DatabasePath)
if ($Execute) {
    $PythonArguments += @('--execute', '--backup-path', $BackupPath)
}
$PythonOutput = & $Python @PythonArguments
if ($LASTEXITCODE -ne 0) {
    throw "Checkpoint migration failed with exit code $LASTEXITCODE."
}
$Result = ($PythonOutput | Select-Object -Last 1) | ConvertFrom-Json -ErrorAction Stop

if ($Execute) {
    if (-not $SkipProcessCheck) {
        Assert-NoDevProcess
    }
    $PlaylistAfter = Get-PlaylistSnapshot $PlaylistPath
    if ($PlaylistBefore -cne $PlaylistAfter) {
        throw 'Playlist storage changed during checkpoint migration. A backup is available.'
    }
    $Result | Add-Member -NotePropertyName playlistUnchanged -NotePropertyValue $true
    $EvidenceDirectory = Split-Path -Parent $EvidencePath
    New-Item -ItemType Directory -Force -Path $EvidenceDirectory | Out-Null
    $Result | ConvertTo-Json -Depth 5 |
        Set-Content -LiteralPath $EvidencePath -Encoding utf8NoBOM
}

Write-Host "Python/SQLite: $PythonVersion"
Write-Host "Database: $DatabasePath"
Write-Host "Action: $($Result.action)"
if ($Result.PSObject.Properties.Name.Contains('backupPath') -and $Result.backupPath) {
    Write-Host "Backup: $($Result.backupPath)"
}
$Result | ConvertTo-Json -Depth 5
