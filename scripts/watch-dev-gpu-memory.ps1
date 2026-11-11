#Requires -Version 7.2
<#
.SYNOPSIS
    Sample GPU process memory for repository audio processes without affecting the safety watchdog.
#>
[CmdletBinding()]
param(
    [int]$ActiveIntervalSeconds = 1,
    [int]$IdleIntervalSeconds = 5,
    [string]$EvidenceDirectory
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$localRoot = Join-Path $root '.local'
if (-not $EvidenceDirectory) {
    $EvidenceDirectory = Join-Path $localRoot ('tests/memory-gpu-live-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
New-Item -ItemType Directory -Path $EvidenceDirectory -Force | Out-Null
$gpuClass = 'Win32_PerfFormattedData_GPUPerformanceCounters_GPUProcessMemory'
$columns = @(
    'utc', 'pid', 'processName', 'adapter', 'adapterInstance', 'luid', 'physicalAdapter',
    'dedicatedUsageBytes', 'sharedUsageBytes', 'totalCommittedBytes',
    'localUsageBytes', 'nonLocalUsageBytes'
)
$csv = Join-Path $EvidenceDirectory 'gpu-samples.csv'

function Get-Targets {
    @(Get-Process -ErrorAction SilentlyContinue | Where-Object {
        try {
            $_.Id -ne $PID -and $_.Path -and
            $_.Path.StartsWith(
                $localRoot + [IO.Path]::DirectorySeparatorChar,
                [StringComparison]::OrdinalIgnoreCase) -and
            $_.ProcessName -match 'foobar|engine|audio.service|glasswave|cavern|renderer|hybris|ffmpeg'
        } catch { $false }
    })
}

function Add-RotatedGpuRows([object[]]$rows) {
    if (-not $rows.Count) { return }
    if ((Test-Path -LiteralPath $csv) -and (Get-Item -LiteralPath $csv).Length -ge 16MB) {
        $archive = Join-Path $EvidenceDirectory ('gpu-samples-' + [DateTime]::UtcNow.Ticks + '.csv')
        Move-Item -LiteralPath $csv -Destination $archive
    }
    $rows | Export-Csv -LiteralPath $csv -NoTypeInformation -Append
}

function New-GpuRecord(
    [string]$utc,
    [int]$processId,
    [string]$processName,
    [string]$adapter,
    [string]$adapterInstance,
    [string]$luid,
    [string]$physicalAdapter,
    [uint64]$dedicatedUsageBytes,
    [uint64]$sharedUsageBytes,
    [uint64]$totalCommittedBytes,
    [uint64]$localUsageBytes,
    [uint64]$nonLocalUsageBytes
) {
    [pscustomobject][ordered]@{
        utc = $utc
        pid = $processId
        processName = $processName
        adapter = $adapter
        adapterInstance = $adapterInstance
        luid = $luid
        physicalAdapter = $physicalAdapter
        dedicatedUsageBytes = $dedicatedUsageBytes
        sharedUsageBytes = $sharedUsageBytes
        totalCommittedBytes = $totalCommittedBytes
        localUsageBytes = $localUsageBytes
        nonLocalUsageBytes = $nonLocalUsageBytes
    }
}

$started = [DateTime]::UtcNow
# Warm the counter provider once before entering the low-latency loop.
[void](Get-CimInstance -Namespace root/cimv2 -ClassName $gpuClass -ErrorAction Stop)
$warmed = [DateTime]::UtcNow
if (-not (Test-Path -LiteralPath $csv)) {
    $csvHeader = $columns -join ','
    Set-Content -LiteralPath $csv -Value $csvHeader -Encoding utf8NoBOM
}
[ordered]@{
    samplerPid = $PID
    startedUtc = $started.ToString('o')
    wmiWarmedUtc = $warmed.ToString('o')
    evidence = $EvidenceDirectory
    gpuClass = $gpuClass
    activeIntervalSeconds = $ActiveIntervalSeconds
    idleIntervalSeconds = $IdleIntervalSeconds
    targetPathRoot = $localRoot
    columns = $columns
} | ConvertTo-Json -Depth 4 |
    Set-Content -LiteralPath (Join-Path $EvidenceDirectory 'sampler.json')

while ($true) {
    if (Test-Path -LiteralPath (Join-Path $EvidenceDirectory 'stop')) { break }
    $targets = Get-Targets
    if ($targets.Count) {
        $sampleUtc = [DateTime]::UtcNow.ToString('o')
        try {
            $gpuRecords = @(Get-CimInstance -Namespace root/cimv2 -ClassName $gpuClass -ErrorAction Stop)
            $gpuByPid = @{}
            foreach ($gpuRecord in $gpuRecords) {
                $match = [regex]::Match($gpuRecord.Name, '^pid_(?<pid>\d+)_(?<adapter>.+)$')
                if (-not $match.Success) { continue }
                $gpuPid = [int]$match.Groups['pid'].Value
                if (-not $gpuByPid.ContainsKey($gpuPid)) {
                    $gpuByPid[$gpuPid] = @()
                }
                $gpuByPid[$gpuPid] += $gpuRecord
            }
            $rows = @()
            foreach ($target in $targets) {
                if (-not $gpuByPid.ContainsKey($target.Id)) {
                    $rows += New-GpuRecord $sampleUtc $target.Id $target.ProcessName `
                        'none' '' '' '' 0 0 0 0 0
                    continue
                }
                foreach ($gpuRecord in $gpuByPid[$target.Id]) {
                    $adapter = [regex]::Match(
                        $gpuRecord.Name, '^pid_\d+_(?<adapter>.+)$').Groups['adapter'].Value
                    $luidMatch = [regex]::Match(
                        $gpuRecord.Name, 'luid_(?<luid>0x[0-9A-Fa-f]+_0x[0-9A-Fa-f]+)')
                    $physicalMatch = [regex]::Match($gpuRecord.Name, '_phys_(?<physical>\d+)$')
                    $rows += New-GpuRecord $sampleUtc $target.Id $target.ProcessName `
                        $adapter $gpuRecord.Name $luidMatch.Groups['luid'].Value `
                        $physicalMatch.Groups['physical'].Value `
                        $gpuRecord.DedicatedUsage $gpuRecord.SharedUsage `
                        $gpuRecord.TotalCommitted $gpuRecord.LocalUsage $gpuRecord.NonLocalUsage
                }
            }
            Add-RotatedGpuRows $rows
        } catch {
            [Console]::Error.WriteLine(
                '{0} gpu_wmi_sample_error {1}' -f [DateTime]::UtcNow.ToString('o'), $_.Exception.Message)
        }
        Start-Sleep -Seconds $ActiveIntervalSeconds
    } else {
        Start-Sleep -Seconds $IdleIntervalSeconds
    }
}
