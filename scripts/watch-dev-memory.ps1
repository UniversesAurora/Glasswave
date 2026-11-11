#Requires -Version 7.2
<#
.SYNOPSIS
    Monitor system commit/kernel memory and the repository audio processes; stop them on pressure.
#>
[CmdletBinding()]
param(
    [int]$IntervalSeconds = 1,
    [int]$MaxHours = 0,
    [string]$EvidenceDirectory,
    [uint32]$DumpProcessId = 0,
    [string]$DumpPath,
    [switch]$Supervisor,
    [switch]$SelfTest,
    [uint32]$SupervisorPid = 0
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$localRoot = Join-Path $root '.local'
if (-not $EvidenceDirectory) {
    $EvidenceDirectory = Join-Path $localRoot ('tests/memory-watch-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
New-Item -ItemType Directory -Path $EvidenceDirectory -Force | Out-Null
$script:diagnosticsWritten = 0
$script:diagnosticLimit = 256
$script:lastDiagnostic = @{}
function Write-Diagnostic([string]$category, [string]$message) {
    try {
        if ($script:diagnosticsWritten -ge $script:diagnosticLimit) { return }
        $now = [DateTime]::UtcNow
        if ($script:lastDiagnostic.ContainsKey($category) -and
            ($now - $script:lastDiagnostic[$category]).TotalSeconds -lt 30) {
            return
        }
        $script:lastDiagnostic[$category] = $now
        $script:diagnosticsWritten++
        $line = '{0} category={1} {2}{3}' -f
            $now.ToString('o'), $category, $message, [Environment]::NewLine
        $stderr = [Console]::OpenStandardError()
        try {
            $bytes = [Text.Encoding]::UTF8.GetBytes($line)
            $stderr.Write($bytes, 0, $bytes.Length)
            $stderr.Flush()
        } finally {
            $stderr.Dispose()
        }
    } catch {}
}
function Write-JsonOutput([string]$path, $value, [string]$category) {
    try {
        $value | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $path
    } catch {
        Write-Diagnostic $category $_.Exception.Message
    }
}
function Write-WatchPointer(
    [uint32]$watcherProcessId,
    [uint32]$supervisorProcessId,
    [string]$startedUtc,
    [string]$status
) {
    $pointerPath = Join-Path $localRoot 'tests/memory-watch-live.json'
    $temporaryPath = $pointerPath + '.tmp.' + $PID
    try {
        [ordered]@{
            schema = 'memory-watch-live/1'
            generation = [IO.Path]::GetFileName($EvidenceDirectory)
            pid = $watcherProcessId
            supervisorPid = if ($supervisorProcessId) { $supervisorProcessId } else { $null }
            dir = $EvidenceDirectory
            startedUtc = $startedUtc
            updatedUtc = [DateTime]::UtcNow.ToString('o')
            status = $status
            intervalSeconds = $IntervalSeconds
            processPrivateLimitBytes = 8GB
            processSharedCommitLimitBytes = 8GB
        } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $temporaryPath
        Move-Item -LiteralPath $temporaryPath -Destination $pointerPath -Force
    } catch {
        Write-Diagnostic 'watch_pointer' $_.Exception.Message
    } finally {
        if (Test-Path -LiteralPath $temporaryPath) {
            try { Remove-Item -LiteralPath $temporaryPath -Force } catch {}
        }
    }
}
function Get-FallbackValue($getter, $fallback) {
    try {
        & $getter
    } catch {
        Write-Diagnostic 'process_record_property' $_.Exception.Message
        $fallback
    }
}
if ($Supervisor -and -not $DumpProcessId) {
    $childIndex = 0
    $claimed = $false
    $pwshPath = (Get-Command pwsh.exe).Source
    $stopPath = Join-Path $EvidenceDirectory 'stop'
    while (-not (Test-Path -LiteralPath $stopPath)) {
        $pointerPath = Join-Path $localRoot 'tests/memory-watch-live.json'
        if ($claimed -and (Test-Path -LiteralPath $pointerPath)) {
            try {
                $currentPointer = Get-Content -LiteralPath $pointerPath -Raw | ConvertFrom-Json
                if ($currentPointer.dir -and
                    -not [string]::Equals(
                        [IO.Path]::GetFullPath($currentPointer.dir),
                        [IO.Path]::GetFullPath($EvidenceDirectory),
                        [StringComparison]::OrdinalIgnoreCase)) {
                    Write-Diagnostic 'supervisor_superseded' (
                        'another watcher generation owns the live pointer')
                    break
                }
            } catch {
                Write-Diagnostic 'watch_pointer_read' $_.Exception.Message
            }
        }
        $childIndex++
        $childOut = Join-Path $EvidenceDirectory ('watchdog-runtime-' + $childIndex.ToString('000') + '.stdout.log')
        $childErr = Join-Path $EvidenceDirectory ('watchdog-runtime-' + $childIndex.ToString('000') + '.stderr.log')
        $child = $null
        try {
            $child = Start-Process -FilePath $pwshPath -WindowStyle Hidden -PassThru `
                -RedirectStandardOutput $childOut -RedirectStandardError $childErr `
                -ArgumentList @(
                    '-NoProfile', '-File', ('"' + $PSCommandPath + '"'),
                    '-EvidenceDirectory', ('"' + $EvidenceDirectory + '"'),
                    '-IntervalSeconds', $IntervalSeconds, '-MaxHours', $MaxHours,
                    '-SupervisorPid', $PID)
            $claimed = $true
            Write-WatchPointer $child.Id $PID ([DateTime]::UtcNow.ToString('o')) 'starting'
            Write-JsonOutput (Join-Path $EvidenceDirectory 'supervisor.json') ([ordered]@{
                supervisorPid = $PID
                childPid = $child.Id
                childIndex = $childIndex
                startedUtc = [DateTime]::UtcNow.ToString('o')
            }) 'supervisor_state'
            Wait-Process -Id $child.Id -ErrorAction Stop
        } catch {
            if (-not $child) {
                Write-Diagnostic 'supervisor_start' $_.Exception.Message
            }
        }
        if (Test-Path -LiteralPath $stopPath) { break }
        Write-Diagnostic 'supervisor_restart' (
            'watcher child exited unexpectedly; restarting after one second')
        Start-Sleep -Seconds 1
    }
    exit
}
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class GlasswaveMemoryWatch {
    [StructLayout(LayoutKind.Sequential)]
    public struct Info {
        public uint cb;
        public UIntPtr CommitTotal, CommitLimit, CommitPeak, PhysicalTotal, PhysicalAvailable;
        public UIntPtr SystemCache, KernelTotal, KernelPaged, KernelNonpaged, PageSize;
        public uint HandleCount, ProcessCount, ThreadCount;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct ProcessMemoryCounters {
        public uint cb;
        public uint PageFaultCount;
        public UIntPtr PeakWorkingSetSize, WorkingSetSize;
        public UIntPtr QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage;
        public UIntPtr QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage;
        public UIntPtr PagefileUsage, PeakPagefileUsage, PrivateUsage;
        public UIntPtr PrivateWorkingSetSize;
        public ulong SharedCommitUsage;
    }
    [DllImport("psapi.dll", SetLastError=true)]
    static extern bool GetPerformanceInfo(ref Info info, uint size);
    [DllImport("psapi.dll", SetLastError=true)]
    static extern bool GetProcessMemoryInfo(IntPtr process, out ProcessMemoryCounters counters, uint size);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    [DllImport("dbghelp.dll", SetLastError=true)]
    static extern bool MiniDumpWriteDump(IntPtr process, uint pid, IntPtr file, uint type,
        IntPtr exception, IntPtr stream, IntPtr callback);
    public static Info Read() {
        Info info = new Info(); info.cb = (uint)Marshal.SizeOf<Info>();
        if (!GetPerformanceInfo(ref info, info.cb)) throw new System.ComponentModel.Win32Exception();
        return info;
    }
    public static bool TryGetProcessMemoryCounters(uint pid, out ProcessMemoryCounters counters) {
        counters = new ProcessMemoryCounters();
        IntPtr process = OpenProcess(0x1010, false, pid);
        if (process == IntPtr.Zero) return false;
        try {
            counters.cb = (uint)Marshal.SizeOf<ProcessMemoryCounters>();
            return GetProcessMemoryInfo(process, out counters, counters.cb);
        } finally { CloseHandle(process); }
    }
    public static bool Dump(uint pid, string path) {
        IntPtr process = OpenProcess(0x450, false, pid);
        if (process == IntPtr.Zero) return false;
        try {
            using (var file = System.IO.File.Create(path)) {
                return MiniDumpWriteDump(process, pid, file.SafeFileHandle.DangerousGetHandle(),
                    0x1824, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
            }
        } finally { CloseHandle(process); }
    }
}
'@
if ($DumpProcessId) {
    if (-not [GlasswaveMemoryWatch]::Dump($DumpProcessId, $DumpPath)) { exit 2 }
    exit
}
function Get-TargetsFromProcesses($processes) {
    @($processes | Where-Object {
        if ($null -eq $_) { return $false }
        try {
            $_.Id -ne $PID -and $_.Path -and
            $_.Path.StartsWith($localRoot + [IO.Path]::DirectorySeparatorChar,
                [StringComparison]::OrdinalIgnoreCase) -and
            $_.ProcessName -match 'foobar|engine|audio.service|glasswave|cavern|renderer|hybris|ffmpeg'
        } catch { $false }
    })
}
function Get-Targets {
    Get-TargetsFromProcesses @(Get-Process -ErrorAction SilentlyContinue)
}
function Global-Record($info, [double]$pageBytes, [string]$utc) {
    $commitBytes = $info.CommitTotal.ToUInt64() * $pageBytes
    [pscustomobject][ordered]@{
        utc=$utc; commitBytes=$commitBytes;
        commitLimitBytes=$info.CommitLimit.ToUInt64()*$pageBytes;
        commitPercent=[math]::Round(
            100.0 * $info.CommitTotal.ToUInt64() / $info.CommitLimit.ToUInt64(), 2);
        availableBytes=$info.PhysicalAvailable.ToUInt64()*$pageBytes;
        physicalTotalBytes=$info.PhysicalTotal.ToUInt64()*$pageBytes;
        cacheBytes=$info.SystemCache.ToUInt64()*$pageBytes;
        kernelTotalBytes=$info.KernelTotal.ToUInt64()*$pageBytes;
        kernelPagedBytes=$info.KernelPaged.ToUInt64()*$pageBytes;
        kernelNonpagedBytes=$info.KernelNonpaged.ToUInt64()*$pageBytes;
        pageSizeBytes=$pageBytes; systemHandleCount=$info.HandleCount;
        systemProcessCount=$info.ProcessCount; systemThreadCount=$info.ThreadCount
    }
}
function Process-Record($process) {
    if ($null -eq $process) { return $null }
    $processId = Get-FallbackValue { $process.Id } $null
    if ($null -eq $processId) { return $null }
    try {
        $counters = [GlasswaveMemoryWatch+ProcessMemoryCounters]::new()
        $hasMemoryCounters = [GlasswaveMemoryWatch]::TryGetProcessMemoryCounters(
            [uint32]$processId, [ref]$counters)
        $record = [ordered]@{
            pid=$processId
            name=Get-FallbackValue { $process.ProcessName } $null
            path=Get-FallbackValue { $process.Path } $null
            privateBytes=Get-FallbackValue { $process.PrivateMemorySize64 } $null
            workingBytes=Get-FallbackValue { $process.WorkingSet64 } $null
            virtualBytes=Get-FallbackValue { $process.VirtualMemorySize64 } $null
            handles=Get-FallbackValue { $process.HandleCount } $null
            threads=Get-FallbackValue { $process.Threads.Count } $null
            pagedMemoryBytes=Get-FallbackValue { $process.PagedMemorySize64 } $null
            peakPagedMemoryBytes=Get-FallbackValue { $process.PeakPagedMemorySize64 } $null
            pagedSystemMemoryBytes=Get-FallbackValue { $process.PagedSystemMemorySize64 } $null
            nonpagedSystemMemoryBytes=Get-FallbackValue { $process.NonpagedSystemMemorySize64 } $null
            cpuSeconds=Get-FallbackValue { $process.CPU } $null; pagefileUsageBytes=$null;
            peakPagefileUsageBytes=$null; privateUsageBytes=$null;
            privateWorkingSetBytes=$null; sharedCommitUsageBytes=$null;
            memoryCountersApi=$null
        }
        if ($hasMemoryCounters) {
            $record.pagefileUsageBytes = $counters.PagefileUsage.ToUInt64()
            $record.peakPagefileUsageBytes = $counters.PeakPagefileUsage.ToUInt64()
            $record.privateUsageBytes = $counters.PrivateUsage.ToUInt64()
            $record.privateWorkingSetBytes = $counters.PrivateWorkingSetSize.ToUInt64()
            $record.sharedCommitUsageBytes = $counters.SharedCommitUsage
            $record.memoryCountersApi = 'GetProcessMemoryInfo:PROCESS_MEMORY_COUNTERS_EX2'
        }
        [pscustomobject]$record
    } catch {
        Write-Diagnostic 'process_record' $_.Exception.Message
        $null
    }
}
function Target-Sample-Record([string]$utc, $record) {
    if ($null -eq $record -or $null -eq $record.pid) { return $null }
    try {
        [pscustomobject][ordered]@{
            utc=$utc; pid=$record.pid; name=$record.name; path=$record.path;
            privateBytes=$record.privateBytes; workingBytes=$record.workingBytes;
            virtualBytes=$record.virtualBytes; handles=$record.handles; threads=$record.threads;
            pagedMemoryBytes=$record.pagedMemoryBytes;
            peakPagedMemoryBytes=$record.peakPagedMemoryBytes;
            pagedSystemMemoryBytes=$record.pagedSystemMemoryBytes;
            nonpagedSystemMemoryBytes=$record.nonpagedSystemMemoryBytes;
            cpuSeconds=$record.cpuSeconds;
            pagefileUsageBytes=$record.pagefileUsageBytes;
            peakPagefileUsageBytes=$record.peakPagefileUsageBytes;
            privateUsageBytes=$record.privateUsageBytes;
            privateWorkingSetBytes=$record.privateWorkingSetBytes;
            sharedCommitUsageBytes=$record.sharedCommitUsageBytes;
            memoryCountersApi=$record.memoryCountersApi
        }
    } catch {
        Write-Diagnostic 'target_record' $_.Exception.Message
        $null
    }
}
function Add-RotatedCsvRows {
    param(
        [string]$Path,
        [object[]]$Rows,
        [string]$Category,
        [switch]$RequirePid
    )
    try {
        $validRows = @()
        foreach ($row in @($Rows)) {
            if ($null -eq $row) { continue }
            if ($RequirePid) {
                $rowPid = $null
                try { $rowPid = $row.pid } catch {}
                if ($null -eq $rowPid -or [int64]$rowPid -le 0) { continue }
            }
            if ($row -is [pscustomobject]) {
                $validRows += $row
            } else {
                $validRows += [pscustomobject]$row
            }
        }
        if (-not $validRows.Count) { return }
        if ((Test-Path -LiteralPath $Path) -and (Get-Item -LiteralPath $Path).Length -ge 16MB) {
            $directory = Split-Path -Parent $Path
            $stem = [IO.Path]::GetFileNameWithoutExtension($Path)
            $archive = Join-Path $directory ($stem + '-' + [DateTime]::UtcNow.Ticks + '.csv')
            Move-Item -LiteralPath $Path -Destination $archive
        }
        $validRows | Export-Csv -LiteralPath $Path -NoTypeInformation -Append
    } catch {
        Write-Diagnostic $Category $_.Exception.Message
    }
}
function New-IncidentDirectory([string]$evidenceDirectory, [string]$utc) {
    $incidentRoot = Join-Path $evidenceDirectory 'incidents'
    New-Item -ItemType Directory -Path $incidentRoot -Force | Out-Null
    $stamp = [DateTimeOffset]::Parse($utc, [Globalization.CultureInfo]::InvariantCulture).
        UtcDateTime.ToString('yyyyMMddTHHmmssfffZ')
    $incidentDirectory = Join-Path $incidentRoot $stamp
    if (Test-Path -LiteralPath $incidentDirectory) {
        $incidentDirectory += '-' + [DateTime]::UtcNow.Ticks
    }
    New-Item -ItemType Directory -Path $incidentDirectory | Out-Null
    $incidentDirectory
}
if ($SelfTest) {
    $noTargets = @(Get-TargetsFromProcesses @())
    $nullTarget = Target-Sample-Record ([DateTime]::UtcNow.ToString('o')) $null
    $staleProbe = Start-Process -FilePath (Get-Command pwsh.exe).Source -WindowStyle Hidden -PassThru `
        -ArgumentList @('-NoProfile', '-Command', 'Start-Sleep -Seconds 30')
    try {
        try { $staleProbe.Kill($true) } catch {}
        try { [void]$staleProbe.WaitForExit(3000) } catch {}
        $staleRecord = Process-Record $staleProbe
    } finally {
        if (-not $staleProbe.HasExited) { try { $staleProbe.Kill($true) } catch {} }
    }
    $blockedPath = Join-Path $EvidenceDirectory (
        'blocked-output-' + [DateTime]::UtcNow.Ticks + '.csv')
    New-Item -ItemType Directory -Path $blockedPath | Out-Null
    Add-RotatedCsvRows -Path $blockedPath -Rows @([pscustomobject]@{
        utc = [DateTime]::UtcNow.ToString('o')
        value = 1
    }) -Category 'blocked_csv_selftest'
    $result = [ordered]@{
        utc = [DateTime]::UtcNow.ToString('o')
        noTargetCount = $noTargets.Count
        nullTargetFiltered = ($null -eq $nullTarget)
        staleTargetHandled = ($null -eq $staleRecord -or $null -ne $staleRecord.pid)
        blockedCsvWriteCaught = ($script:diagnosticsWritten -gt 0)
        diagnosticsWritten = $script:diagnosticsWritten
    }
    Write-JsonOutput (Join-Path $EvidenceDirectory 'selftest-result.json') $result 'selftest_result'
    if ($result.noTargetCount -ne 0 -or
        -not $result.nullTargetFiltered -or
        -not $result.staleTargetHandled -or
        -not $result.blockedCsvWriteCaught) {
        exit 1
    }
    exit 0
}
$started = [DateTime]::UtcNow
Write-WatchPointer $PID $SupervisorPid $started.ToString('o') 'running'
$baseline = [GlasswaveMemoryWatch]::Read()
$pageBytes = [double]$baseline.PageSize.ToUInt64()
$minimumAvailable = [math]::Max(2GB, $baseline.PhysicalTotal.ToUInt64() * $pageBytes * 0.08)
$csv = Join-Path $EvidenceDirectory 'samples.csv'
$targetCsv = Join-Path $EvidenceDirectory 'target-samples.csv'
Write-JsonOutput (Join-Path $EvidenceDirectory 'watcher.json') ([ordered]@{
    watcherPid=$PID; startedUtc=$started.ToString('o'); evidence=$EvidenceDirectory;
    outputSchema=1; dumpMask='0x1824';
    intervalSeconds=$IntervalSeconds; durationHours=$MaxHours; commitLimitPercent=85; minimumAvailableBytes=$minimumAvailable;
    processPrivateLimitBytes=8GB; processSharedCommitLimitBytes=8GB;
    kernelPoolGrowthLimitBytes=1GB
}) 'watcher_json'
$consecutiveIterationFailures = 0
while ($MaxHours -le 0 -or ([DateTime]::UtcNow - $started).TotalHours -lt $MaxHours) {
    try {
        if (Test-Path -LiteralPath (Join-Path $EvidenceDirectory 'stop')) { break }
        $info = [GlasswaveMemoryWatch]::Read()
        $targets = @(Get-Targets)
        $records = @($targets | ForEach-Object {
            $record = Process-Record $_
            if ($null -ne $record -and $null -ne $record.pid) { $record }
        })
        $commitBytes = $info.CommitTotal.ToUInt64() * $pageBytes
        $commitPercent = 100.0 * $info.CommitTotal.ToUInt64() / $info.CommitLimit.ToUInt64()
        $availableBytes = $info.PhysicalAvailable.ToUInt64() * $pageBytes
        $kernelPaged = $info.KernelPaged.ToUInt64() * $pageBytes
        $kernelNonpaged = $info.KernelNonpaged.ToUInt64() * $pageBytes
        $reason = @()
        if ($commitPercent -ge 85) { $reason += 'system_commit_pressure' }
        if ($availableBytes -le $minimumAvailable) { $reason += 'physical_memory_pressure' }
        if (@($records | Where-Object { $null -ne $_.privateBytes -and $_.privateBytes -ge 8GB }).Count) {
            $reason += 'audio_process_private_limit'
        }
        if (@($records | Where-Object {
            $null -ne $_.sharedCommitUsageBytes -and $_.sharedCommitUsageBytes -ge 8GB
        }).Count) {
            $reason += 'audio_process_shared_commit_limit'
        }
        if ($kernelPaged - $baseline.KernelPaged.ToUInt64()*$pageBytes -ge 1GB -or
            $kernelNonpaged - $baseline.KernelNonpaged.ToUInt64()*$pageBytes -ge 1GB) {
            $reason += 'kernel_pool_growth'
        }
        $rowUtc = [DateTime]::UtcNow.ToString('o')
        $row = Global-Record $info $pageBytes $rowUtc
        $targetPrivateSum = 0
        $targetWorkingSum = 0
        if ($records.Count) {
            $privateMeasure = $records.privateBytes | Measure-Object -Sum
            $workingMeasure = $records.workingBytes | Measure-Object -Sum
            if ($null -ne $privateMeasure.Sum) { $targetPrivateSum = $privateMeasure.Sum }
            if ($null -ne $workingMeasure.Sum) { $targetWorkingSum = $workingMeasure.Sum }
        }
        $row | Add-Member -NotePropertyMembers ([ordered]@{
            targetPrivateBytes=$targetPrivateSum;
            targetWorkingBytes=$targetWorkingSum;
            targetPids=($records.pid -join ':'); targetCount=$records.Count
        })
        Add-RotatedCsvRows -Path $csv -Rows @($row) -Category 'samples_csv'
        $targetRows = @($records | ForEach-Object {
            $targetRow = Target-Sample-Record $rowUtc $_
            if ($null -ne $targetRow) { $targetRow }
        })
        Add-RotatedCsvRows -Path $targetCsv -Rows $targetRows -Category 'target_samples_csv' -RequirePid
        if ($reason.Count -and $targets.Count) {
            try {
                $largestProcesses = @(Get-Process | Sort-Object PrivateMemorySize64 -Descending |
                    Select-Object -First 25 | ForEach-Object { Process-Record $_ })
            } catch {
                Write-Diagnostic 'alert_process_snapshot' $_.Exception.Message
                $largestProcesses = @()
            }
            $incidentDirectory = $null
            try {
                $incidentDirectory = New-IncidentDirectory $EvidenceDirectory $row.utc
            } catch {
                Write-Diagnostic 'incident_directory' $_.Exception.Message
            }
            if (-not $incidentDirectory) {
                try {
                    $incidentDirectory = Join-Path $EvidenceDirectory (
                        'emergency-incident-' + [DateTime]::UtcNow.Ticks)
                    New-Item -ItemType Directory -Path $incidentDirectory | Out-Null
                } catch {
                    Write-Diagnostic 'emergency_incident_directory' $_.Exception.Message
                    $incidentDirectory = $EvidenceDirectory
                }
            }
            Write-JsonOutput (Join-Path $incidentDirectory 'alert.json') ([ordered]@{
                utc=$row.utc; reasons=$reason; sample=$row; targets=$records;
                largestProcesses=$largestProcesses
            }) 'alert_json'
            $stopIndex = 0
            foreach ($target in $targets) {
                $stopIndex++
                $targetId = Get-FallbackValue { $target.Id } $null
                $targetName = Get-FallbackValue { $target.ProcessName } 'unknown'
                if ($null -eq $targetId) { continue }
                $dumpPath = Join-Path $incidentDirectory ($targetName + '-' + $targetId + '.dmp')
                $dumpCompleted = $false
                $dumpExitCode = $null
                $dumpFileBytes = $null
                $dumper = $null
                try {
                    $dumper = Start-Process -FilePath (Get-Command pwsh.exe).Source -WindowStyle Hidden -PassThru `
                        -ArgumentList @('-NoProfile', '-File', ('"' + $PSCommandPath + '"'),
                            '-DumpProcessId', $targetId, '-DumpPath', ('"' + $dumpPath + '"'),
                            '-EvidenceDirectory', ('"' + $incidentDirectory + '"'))
                    $dumpCompleted = $dumper.WaitForExit(2000)
                    if (-not $dumpCompleted) { try { $dumper.Kill($true) } catch {} }
                    if ($dumpCompleted) {
                        try { $dumpExitCode = $dumper.ExitCode } catch {}
                    }
                    if (Test-Path -LiteralPath $dumpPath) {
                        $dumpFileBytes = (Get-Item -LiteralPath $dumpPath).Length
                    }
                } catch {
                    Write-Diagnostic 'dump_capture' $_.Exception.Message
                }
                Write-JsonOutput (
                    Join-Path $incidentDirectory ('dump-' + $stopIndex.ToString('00') + '-' + $targetId + '.json')
                ) ([ordered]@{
                    utc=[DateTime]::UtcNow.ToString('o'); pid=$targetId; processName=$targetName;
                    dumpPath=$dumpPath; completed=$dumpCompleted; exitCode=$dumpExitCode;
                    dumpFileBytes=$dumpFileBytes
                }) 'dump_status_json'

                $killError = $null
                try { $target.Kill($true) } catch { $killError = $_.Exception.Message }
                $stopUtc = [DateTime]::UtcNow.ToString('o')
                $stopInfo = $null
                try {
                    $stopInfo = [GlasswaveMemoryWatch]::Read()
                } catch {
                    Write-Diagnostic 'stop_global_read' $_.Exception.Message
                }
                $stopGlobal = if ($stopInfo) { Global-Record $stopInfo $pageBytes $stopUtc } else { $null }
                Write-JsonOutput (
                    Join-Path $incidentDirectory ('stop-' + $stopIndex.ToString('00') + '-' + $targetId + '.json')
                ) ([ordered]@{
                    utc=$stopUtc; stoppedPid=$targetId; processName=$targetName;
                    killError=$killError; global=$stopGlobal
                }) 'stop_json'
                try {
                    $survivingTargets = @($targets | ForEach-Object {
                        $live = Get-Process -Id $_.Id -ErrorAction SilentlyContinue
                        if ($live) { Process-Record $live }
                    })
                    $stopLargestProcesses = @(Get-Process |
                        Sort-Object PrivateMemorySize64 -Descending |
                        Select-Object -First 25 | ForEach-Object { Process-Record $_ })
                    Write-JsonOutput (
                        Join-Path $incidentDirectory (
                            'stop-processes-' + $stopIndex.ToString('00') + '-' + $targetId + '.json')
                    ) ([ordered]@{
                        utc=[DateTime]::UtcNow.ToString('o'); survivingTargets=$survivingTargets;
                        largestProcesses=$stopLargestProcesses
                    }) 'stop_processes_json'
                } catch {
                    Write-Diagnostic 'stop_process_snapshot' $_.Exception.Message
                }
            }
            $after = $null
            try {
                $after = [GlasswaveMemoryWatch]::Read()
            } catch {
                Write-Diagnostic 'after_global_read' $_.Exception.Message
            }
            if ($after) {
                $afterUtc = [DateTime]::UtcNow.ToString('o')
                $afterGlobal = Global-Record $after $pageBytes $afterUtc
                Write-JsonOutput (Join-Path $incidentDirectory 'after-stop.json') ([ordered]@{
                    utc=$afterUtc; global=$afterGlobal
                }) 'after_stop_json'
                try {
                    $afterLargestProcesses = @(Get-Process |
                        Sort-Object PrivateMemorySize64 -Descending |
                        Select-Object -First 25 | ForEach-Object { Process-Record $_ })
                    Write-JsonOutput (Join-Path $incidentDirectory 'after-processes.json') ([ordered]@{
                        utc=[DateTime]::UtcNow.ToString('o')
                        largestProcesses=$afterLargestProcesses
                    }) 'after_processes_json'
                } catch {
                    Write-Diagnostic 'after_process_snapshot' $_.Exception.Message
                }
                $baseline = $after
            }
        }
        $consecutiveIterationFailures = 0
    } catch {
        $consecutiveIterationFailures++
        Write-Diagnostic 'main_iteration' (
            'failure={0}; consecutive={1}' -f $_.Exception.Message, $consecutiveIterationFailures)
    }
    Start-Sleep -Seconds $IntervalSeconds
}
