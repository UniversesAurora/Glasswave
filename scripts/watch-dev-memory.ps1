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
    # Writes one set of incident forensics (sections, pool tags, log tails) and exits.
    [switch]$ForensicsTest,
    # Run the supervisor without asking for elevation (forensics then see less).
    [switch]$NoElevate,
    # A trial watcher (tests): leaves the live watchdog's pointer file alone.
    [switch]$Trial,
    [uint32]$SupervisorPid = 0,
    # A rise of this much commit within RiseWindowSeconds captures forensics without stopping
    # anything, so the growth itself is recorded before the stop threshold is reached.
    [double]$RiseBytes = 4GB,
    [int]$RiseWindowSeconds = 10,
    # The stop thresholds and the repository processes they stop (tests point them elsewhere).
    # This machine idles at 81-85 % commit with the player using under 1 GB, so a level alone
    # stops only near exhaustion; earlier, a level is a stop only while commit is rising fast.
    [double]$CommitPercentLimit = 95,
    [double]$RisingCommitPercentLimit = 88,
    [string]$TargetPattern = 'foobar|engine|audio.service|glasswave|cavern|renderer|hybris|ffmpeg'
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
    if ($Trial) { return }
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
$elevated = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).
    IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
# The forensics need administrator rights: unelevated, Windows hides kernel object addresses
# and refuses the handles of services, so sections could not be told apart or sized. The
# supervisor asks for elevation once (UAC) and its watcher inherits it.
if ($Supervisor -and -not $DumpProcessId -and -not $elevated -and -not $NoElevate) {
    $arguments = @('-NoProfile', '-File', ('"' + $PSCommandPath + '"'))
    foreach ($name in $PSBoundParameters.Keys) {
        $value = $PSBoundParameters[$name]
        if ($value -is [switch]) { if ($value) { $arguments += '-' + $name } }
        else { $arguments += '-' + $name; $arguments += ('"' + $value + '"') }
    }
    Start-Process -FilePath (Get-Command pwsh.exe).Source -Verb RunAs -WindowStyle Hidden -ArgumentList $arguments
    exit
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
                    '-RiseBytes', $RiseBytes, '-RiseWindowSeconds', $RiseWindowSeconds,
                    '-CommitPercentLimit', $CommitPercentLimit,
                    '-RisingCommitPercentLimit', $RisingCommitPercentLimit,
                    '-TargetPattern', ('"' + $TargetPattern + '"'),
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
# Evidence for commit that no process's counters show (2026-11-14: spikes of 14-19 GB released
# when engine.exe exits, while neither it nor foobar2000 held them in its address space):
# every Section object in the system with its size, kind and holders, and the kernel pools by
# tag. Buffers are allocated up front so a capture at 99 % commit needs little more.
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
public static class GlasswaveMemoryForensics {
    [StructLayout(LayoutKind.Sequential)]
    struct SectionBasic { public IntPtr BaseAddress; public uint AllocationAttributes; public long MaximumSize; }
    [DllImport("ntdll.dll")] static extern int NtQuerySystemInformation(int cls, IntPtr buffer, int length, out int returned);
    [DllImport("ntdll.dll")] static extern int NtQuerySection(IntPtr section, int cls, out SectionBasic info, IntPtr length, out IntPtr returned);
    [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool DuplicateHandle(IntPtr sourceProcess, IntPtr source, IntPtr targetProcess, out IntPtr target,
        uint access, bool inherit, uint options);
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    static extern IntPtr CreateFileMappingW(IntPtr file, IntPtr attributes, uint protect, uint high, uint low, string name);

    const uint SecFile = 0x800000, SecImage = 0x1000000, SecReserve = 0x4000000;
    static IntPtr s_buffer = IntPtr.Zero;
    static int s_size;
    static int s_sectionType = -1;
    static IntPtr s_probe = IntPtr.Zero;

    struct Entry { public long Object; public uint Pid; public long Handle; public ushort Type; }

    [DllImport("ntdll.dll")] static extern int RtlAdjustPrivilege(int privilege, bool enable, bool thread, out bool previous);
    static bool s_debug;

    public static void Prepare() {
        Grow(96 << 20);
        if (s_probe == IntPtr.Zero) s_probe = CreateFileMappingW(new IntPtr(-1), IntPtr.Zero, 4, 0, 4096, null);
        // SeDebugPrivilege (20): handles of every process, services included, can be duplicated.
        // Granted only when the watcher runs elevated.
        bool previous;
        if (!s_debug) s_debug = RtlAdjustPrivilege(20, true, false, out previous) == 0;
    }
    static void Grow(int size) {
        if (size <= s_size) return;
        if (s_buffer != IntPtr.Zero) Marshal.FreeHGlobal(s_buffer);
        s_buffer = Marshal.AllocHGlobal(size);
        s_size = size;
    }
    static void Query(int cls) {
        for (int attempt = 0; attempt < 8; attempt++) {
            int returned;
            int status = NtQuerySystemInformation(cls, s_buffer, s_size, out returned);
            if (status == 0) return;
            if ((uint)status != 0xC0000004) throw new Exception("NtQuerySystemInformation(" + cls + ") 0x" + status.ToString("X8"));
            Grow(Math.Max(returned + (4 << 20), s_size * 2));
        }
        throw new Exception("NtQuerySystemInformation(" + cls + ") kept growing");
    }
    static List<Entry> Handles() {
        Query(64); // SystemExtendedHandleInformation: count, reserved, then 40-byte entries
        long count = Marshal.ReadInt64(s_buffer);
        var list = new List<Entry>((int)Math.Min(count, 4000000));
        for (long i = 0; i < count; i++) {
            IntPtr e = IntPtr.Add(s_buffer, (int)(16 + i * 40));
            list.Add(new Entry { Object = Marshal.ReadInt64(e), Pid = (uint)Marshal.ReadInt64(e, 8),
                                 Handle = Marshal.ReadInt64(e, 16), Type = (ushort)Marshal.ReadInt16(e, 30) });
        }
        return list;
    }
    static string Name(uint pid, Dictionary<uint, string> names) {
        string name;
        if (names.TryGetValue(pid, out name)) return name;
        try { name = pid == 4 ? "System" : Process.GetProcessById((int)pid).ProcessName; } catch { name = "?"; }
        names[pid] = name;
        return name;
    }

    // Every Section object: size and kind (pagefile-backed unless SEC_FILE or SEC_IMAGE) and the
    // processes holding handles to it; a section held by several processes is counted once.
    public static string Sections(int top) {
        Prepare();
        var watch = Stopwatch.StartNew();
        var all = Handles();
        uint self = (uint)Process.GetCurrentProcess().Id;
        if (s_sectionType < 0)
            foreach (var e in all)
                if (e.Pid == self && e.Handle == s_probe.ToInt64()) { s_sectionType = e.Type; break; }
        if (s_sectionType < 0) return "section type unknown (probe handle not found)\n";
        // Unelevated, Windows hides kernel object addresses (all 0): every handle then counts as
        // a section of its own, and a section shared by several processes counts several times.
        var byObject = new Dictionary<long, List<Entry>>();
        long hidden = 0;
        foreach (var e in all) {
            if (e.Type != s_sectionType) continue;
            long key = e.Object != 0 ? e.Object : --hidden;
            List<Entry> holders;
            if (!byObject.TryGetValue(key, out holders)) byObject[key] = holders = new List<Entry>();
            holders.Add(e);
        }
        var processes = new Dictionary<uint, IntPtr>();
        var names = new Dictionary<uint, string>();
        var rows = new List<Tuple<long, uint, List<Entry>>>(); // size, attributes, holders
        int unknown = 0;
        foreach (var pair in byObject) {
            long size = -1; uint attributes = 0;
            foreach (var holder in pair.Value) {
                IntPtr process;
                if (!processes.TryGetValue(holder.Pid, out process)) processes[holder.Pid] = process = OpenProcess(0x40, false, holder.Pid);
                if (process == IntPtr.Zero) continue;
                IntPtr local;
                if (!DuplicateHandle(process, new IntPtr(holder.Handle), GetCurrentProcess(), out local, 0x1, false, 0)) continue;
                SectionBasic info; IntPtr returned;
                if (NtQuerySection(local, 0, out info, new IntPtr(Marshal.SizeOf(typeof(SectionBasic))), out returned) == 0) {
                    size = info.MaximumSize; attributes = info.AllocationAttributes;
                }
                CloseHandle(local);
                if (size >= 0) break;
            }
            if (size < 0) unknown++;
            rows.Add(Tuple.Create(size, attributes, pair.Value));
        }
        foreach (var process in processes.Values) if (process != IntPtr.Zero) CloseHandle(process);
        rows.Sort((a, b) => b.Item1.CompareTo(a.Item1));
        // A pagefile-backed SEC_COMMIT section charges its whole size to system commit when it
        // is created; a SEC_RESERVE one (the .NET GC's 2 TB regions) only what is committed in it.
        long pagefile = 0, reserve = 0, file = 0, image = 0; int pagefileCount = 0;
        var perProcess = new Dictionary<uint, long>();
        foreach (var row in rows) {
            if (row.Item1 < 0) continue;
            bool isFile = (row.Item2 & SecFile) != 0, isImage = (row.Item2 & SecImage) != 0;
            if (isImage) image += row.Item1; else if (isFile) file += row.Item1;
            else if ((row.Item2 & SecReserve) != 0) reserve += row.Item1;
            else {
                pagefile += row.Item1; pagefileCount++;
                var seen = new HashSet<uint>();
                foreach (var holder in row.Item3)
                    if (seen.Add(holder.Pid)) { long sum; perProcess.TryGetValue(holder.Pid, out sum); perProcess[holder.Pid] = sum + row.Item1; }
            }
        }
        int sectionHandles = 0;
        foreach (var row in rows) sectionHandles += row.Item3.Count;
        var text = new StringBuilder();
        text.AppendFormat("handles {0}, section handles {1}, sections {2} (size unknown {3}), {4} ms, debug privilege {5}\n",
            all.Count, sectionHandles, rows.Count, unknown, watch.ElapsedMilliseconds, s_debug);
        text.AppendFormat("pagefile-backed, committed (SEC_COMMIT): {0:N0} MB in {1} sections; reserved (SEC_RESERVE): {2:N0} MB; file: {3:N0} MB; image: {4:N0} MB\n",
            pagefile / 1048576.0, pagefileCount, reserve / 1048576.0, file / 1048576.0, image / 1048576.0);
        text.Append("largest committed pagefile-backed sections (MB, attributes, holders):\n");
        int shown = 0;
        foreach (var row in rows) {
            if (row.Item1 < 0 || (row.Item2 & (SecFile | SecImage | SecReserve)) != 0) continue;
            if (shown++ >= top) break;
            var holders = new Dictionary<uint, int>();
            foreach (var holder in row.Item3) { int n; holders.TryGetValue(holder.Pid, out n); holders[holder.Pid] = n + 1; }
            var list = new List<string>();
            foreach (var h in holders) list.Add(Name(h.Key, names) + ":" + h.Key + (h.Value > 1 ? "x" + h.Value : ""));
            text.AppendFormat("  {0,10:N1}  0x{1:X8}  {2}\n", row.Item1 / 1048576.0, row.Item2, string.Join(", ", list));
        }
        text.Append("committed pagefile-backed sections held, by process (MB):\n");
        var order = new List<KeyValuePair<uint, long>>(perProcess);
        order.Sort((a, b) => b.Value.CompareTo(a.Value));
        for (int i = 0; i < Math.Min(top, order.Count); i++)
            text.AppendFormat("  {0,10:N1}  {1}:{2}\n", order[i].Value / 1048576.0, Name(order[i].Key, names), order[i].Key);
        return text.ToString();
    }

    // Kernel pool use by tag (SystemPoolTagInformation), largest first.
    public static string PoolTags(int top) {
        Prepare();
        Query(22);
        int count = Marshal.ReadInt32(s_buffer);
        var rows = new List<Tuple<long, long, string>>();
        for (int i = 0; i < count; i++) {
            IntPtr e = IntPtr.Add(s_buffer, 8 + i * 40);
            var tag = new byte[4]; Marshal.Copy(e, tag, 0, 4);
            long paged = Marshal.ReadInt64(e, 16), nonpaged = Marshal.ReadInt64(e, 32);
            rows.Add(Tuple.Create(paged, nonpaged, Encoding.ASCII.GetString(tag)));
        }
        rows.Sort((a, b) => (b.Item1 + b.Item2).CompareTo(a.Item1 + a.Item2));
        var text = new StringBuilder("tag   paged MB   nonpaged MB\n");
        for (int i = 0; i < Math.Min(top, rows.Count); i++)
            text.AppendFormat("{0}  {1,9:N1}  {2,12:N1}\n", rows[i].Item3, rows[i].Item1 / 1048576.0, rows[i].Item2 / 1048576.0);
        return text.ToString();
    }
}
'@
function Write-Forensics([string]$directory, [string]$suffix) {
    $parts = [ordered]@{
        ('sections' + $suffix + '.txt') = { [GlasswaveMemoryForensics]::Sections(30) }
        ('pool-tags' + $suffix + '.txt') = { [GlasswaveMemoryForensics]::PoolTags(30) }
    }
    foreach ($name in $parts.Keys) {
        try {
            Set-Content -LiteralPath (Join-Path $directory $name) -Value (& $parts[$name])
        } catch {
            Write-Diagnostic ('forensics_' + $name) $_.Exception.Message
        }
    }
}
# The last lines of the logs that say what playback and the Apple Music engine were doing.
function Write-LogTails([string]$directory) {
    $logs = [ordered]@{
        'engine-log-tail.txt' = Join-Path $localRoot 'applemusic/windows/runtime/logs/component-service.log'
        'glasswave-log-tail.txt' = Join-Path $localRoot 'fb2k-dev/profile/glasswave/glasswave.log'
        'playback-log-tail.txt' = Join-Path $localRoot 'fb2k-dev/profile/glasswave/applemusic/playback.log'
    }
    foreach ($name in $logs.Keys) {
        try {
            if (Test-Path -LiteralPath $logs[$name]) {
                Get-Content -LiteralPath $logs[$name] -Tail 400 | Set-Content -LiteralPath (Join-Path $directory $name)
            }
        } catch {
            Write-Diagnostic ('log_tail_' + $name) $_.Exception.Message
        }
    }
}
# Every process's commit (its pagefile usage), largest first, from one process snapshot.
function Process-Commit-Records($processes, [int]$top) {
    @($processes | ForEach-Object {
        try { [pscustomobject]@{ pid = $_.Id; name = $_.ProcessName; commitBytes = $_.PagedMemorySize64 } } catch {}
    } | Sort-Object commitBytes -Descending | Select-Object -First $top)
}
try { [GlasswaveMemoryForensics]::Prepare() } catch { Write-Diagnostic 'forensics_prepare' $_.Exception.Message }
if ($ForensicsTest) {
    $directory = Join-Path $EvidenceDirectory ('forensics-test-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ'))
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    $watch = [Diagnostics.Stopwatch]::StartNew()
    Write-Forensics $directory ''
    Write-LogTails $directory
    'forensics written to {0} in {1} ms' -f $directory, $watch.ElapsedMilliseconds
    exit
}
function Get-TargetsFromProcesses($processes) {
    @($processes | Where-Object {
        if ($null -eq $_) { return $false }
        # The name first: reading Path enumerates the process's modules, which elevated
        # succeeds for every process and took the loop from 1 to 4 seconds.
        try {
            $_.ProcessName -match $TargetPattern -and $_.Id -ne $PID -and $_.Path -and
            $_.Path.StartsWith($localRoot + [IO.Path]::DirectorySeparatorChar,
                [StringComparison]::OrdinalIgnoreCase)
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
function New-IncidentDirectory([string]$evidenceDirectory, [string]$utc, [string]$suffix = '') {
    $incidentRoot = Join-Path $evidenceDirectory 'incidents'
    New-Item -ItemType Directory -Path $incidentRoot -Force | Out-Null
    $stamp = [DateTimeOffset]::Parse($utc, [Globalization.CultureInfo]::InvariantCulture).
        UtcDateTime.ToString('yyyyMMddTHHmmssfffZ') + $suffix
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
    outputSchema=2; dumpMask='0x1824'; elevated=$elevated;
    riseBytes=$RiseBytes; riseWindowSeconds=$RiseWindowSeconds;
    intervalSeconds=$IntervalSeconds; durationHours=$MaxHours; commitLimitPercent=$CommitPercentLimit;
    risingCommitLimitPercent=$RisingCommitPercentLimit;
    targetPattern=$TargetPattern; minimumAvailableBytes=$minimumAvailable;
    processPrivateLimitBytes=8GB; processSharedCommitLimitBytes=8GB;
    kernelPoolGrowthLimitBytes=1GB
}) 'watcher_json'
$consecutiveIterationFailures = 0
$recent = [Collections.Generic.Queue[object]]::new()
$lastRiseCapture = [DateTime]::MinValue
while ($MaxHours -le 0 -or ([DateTime]::UtcNow - $started).TotalHours -lt $MaxHours) {
    try {
        if (Test-Path -LiteralPath (Join-Path $EvidenceDirectory 'stop')) { break }
        $info = [GlasswaveMemoryWatch]::Read()
        $allProcesses = @(Get-Process -ErrorAction SilentlyContinue)
        $targets = @(Get-TargetsFromProcesses $allProcesses)
        $records = @($targets | ForEach-Object {
            $record = Process-Record $_
            if ($null -ne $record -and $null -ne $record.pid) { $record }
        })
        $commitBytes = $info.CommitTotal.ToUInt64() * $pageBytes
        $commitPercent = 100.0 * $info.CommitTotal.ToUInt64() / $info.CommitLimit.ToUInt64()
        $availableBytes = $info.PhysicalAvailable.ToUInt64() * $pageBytes
        $kernelPaged = $info.KernelPaged.ToUInt64() * $pageBytes
        $kernelNonpaged = $info.KernelNonpaged.ToUInt64() * $pageBytes
        # Commit growth within the rise window, for the rising-pressure stop and the rise capture.
        $recent.Enqueue([pscustomobject]@{ t = [DateTime]::UtcNow; commit = $commitBytes })
        while ($recent.Count -and ([DateTime]::UtcNow - $recent.Peek().t).TotalSeconds -gt $RiseWindowSeconds) {
            [void]$recent.Dequeue()
        }
        $windowLow = ($recent | Measure-Object commit -Minimum).Minimum
        $rising = $commitBytes - $windowLow -ge $RiseBytes
        $reason = @()
        if ($commitPercent -ge $CommitPercentLimit) { $reason += 'system_commit_pressure' }
        if ($rising -and $commitPercent -ge $RisingCommitPercentLimit) { $reason += 'system_commit_rising' }
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
        # Commit that no process's own counters hold (sections without views, kernel, drivers).
        $processCommit = 0
        try { $processCommit = ($allProcesses | Measure-Object PagedMemorySize64 -Sum).Sum } catch {}
        $row | Add-Member -NotePropertyMembers ([ordered]@{
            targetPrivateBytes=$targetPrivateSum;
            targetWorkingBytes=$targetWorkingSum;
            targetPids=($records.pid -join ':'); targetCount=$records.Count
            processCommitBytes=$processCommit
            unattributedCommitBytes=($commitBytes - $processCommit)
        })
        Add-RotatedCsvRows -Path $csv -Rows @($row) -Category 'samples_csv'
        # A fast rise is recorded while it happens, without stopping anything (once a minute).
        if ($rising -and ([DateTime]::UtcNow - $lastRiseCapture).TotalSeconds -ge 60) {
            $lastRiseCapture = [DateTime]::UtcNow
            try {
                $riseDirectory = New-IncidentDirectory $EvidenceDirectory $row.utc '-rise'
                Write-JsonOutput (Join-Path $riseDirectory 'rise.json') ([ordered]@{
                    utc=$row.utc; windowSeconds=$RiseWindowSeconds; windowLowCommitBytes=$windowLow
                    riseBytes=($commitBytes - $windowLow); sample=$row; targets=$records
                    processCommit=(Process-Commit-Records $allProcesses 30)
                }) 'rise_json'
                Write-Forensics $riseDirectory ''
                Write-LogTails $riseDirectory
            } catch {
                Write-Diagnostic 'rise_capture' $_.Exception.Message
            }
        }
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
                processCommit=(Process-Commit-Records $allProcesses 30)
            }) 'alert_json'
            # Where the commit is while it is still there, before anything is stopped.
            Write-Forensics $incidentDirectory ''
            Write-LogTails $incidentDirectory
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
            # The same census once they are stopped: what vanished is what they held or caused.
            Write-Forensics $incidentDirectory '-after'
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
