#Requires -Version 7.2
<#
.SYNOPSIS
    Run the component-owned real image-cache regression probe against a loopback mock.

.DESCRIPTION
    Starts tests/image_cache_mock.mjs on a random loopback port, writes the marker under the
    repository dev profile, starts the dev instance, waits for the initquit probe, validates
    component and mock counters, then removes the marker and stops the owned processes.

    The probe uses only cache-image-test keys. It removes those exact keys and never clears
    the shared Image store or another provider's scope.

.PARAMETER Deploy
    Deploy the current Release build through scripts/run-dev.ps1 before launching.

#>
[CmdletBinding()]
param(
    [switch]$Deploy,
    [ValidateRange(10, 180)]
    [int]$TimeoutSeconds = 60
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Instance = Join-Path $Root '.local/fb2k-dev'
$Exe = Join-Path $Instance 'foobar2000.exe'
$MarkerDir = Join-Path $Instance 'profile/glasswave/cache-image-test'
$RequestPath = Join-Path $MarkerDir 'request.json'
$ClaimedPath = Join-Path $MarkerDir 'request.claimed.json'
$ProbeResultPath = Join-Path $MarkerDir 'result.json'
$MockScript = Join-Path $PSScriptRoot '../tests/image_cache_mock.mjs'
$TestBase = Join-Path $Root '.local/tests'
$RunId = 'imagecache-' + [Guid]::NewGuid().ToString('N')
$TestRoot = Join-Path $TestBase $RunId
$ControlPath = Join-Path $TestRoot 'mock-control.json'
$CountersPath = Join-Path $TestRoot 'mock-counters.json'
$MockStdout = Join-Path $TestRoot 'mock-stdout.log'
$MockStderr = Join-Path $TestRoot 'mock-stderr.log'
$FinalResultPath = Join-Path $TestRoot 'result.json'

$mockProcess = $null
$startedDevInstance = $false
$baseUrl = $null

function Stop-DevInstance {
    if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) { return }
    $resolvedExe = (Resolve-Path -LiteralPath $Exe).Path
    $processes = @(
        Get-Process -Name foobar2000 -ErrorAction SilentlyContinue |
            Where-Object { $_.Path -eq $resolvedExe }
    )
    foreach ($process in $processes) {
        [void]$process.CloseMainWindow()
        if (-not $process.WaitForExit(8000)) {
            $process.Kill()
            [void]$process.WaitForExit(3000)
        }
    }
}

function Stop-MockServer {
    if ($null -eq $mockProcess) { return }
    try {
        if (-not $mockProcess.HasExited -and $null -ne $baseUrl) {
            Invoke-RestMethod -Uri "$baseUrl/__shutdown" -TimeoutSec 3 | Out-Null
        }
    } catch {
    }
    if (-not $mockProcess.WaitForExit(3000)) {
        $mockProcess.Kill()
        [void]$mockProcess.WaitForExit(3000)
    }
}

if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) {
    throw 'Dev instance missing; bootstrap is outside this probe.'
}
if (-not (Test-Path -LiteralPath $MockScript -PathType Leaf)) {
    throw "Image-cache mock is missing: $MockScript"
}
$node = Get-Command node -CommandType Application -ErrorAction Stop | Select-Object -First 1

New-Item -ItemType Directory -Force -Path $MarkerDir, $TestRoot | Out-Null
Remove-Item -LiteralPath $RequestPath, $ClaimedPath, $ProbeResultPath -Force -ErrorAction SilentlyContinue

try {
    $nodeArguments = @(
        ('"{0}"' -f $MockScript),
        '--control',
        ('"{0}"' -f $ControlPath),
        '--counters',
        ('"{0}"' -f $CountersPath)
    )
    $mockProcess = Start-Process -FilePath $node.Source -ArgumentList $nodeArguments `
        -WorkingDirectory $Root -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput $MockStdout -RedirectStandardError $MockStderr

    $deadline = [DateTime]::UtcNow.AddSeconds(8)
    $control = $null
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($mockProcess.HasExited) {
            $message = if (Test-Path -LiteralPath $MockStderr) {
                Get-Content -LiteralPath $MockStderr -Raw
            } else {
                'no diagnostic output'
            }
            throw "Image-cache mock exited during startup: $message"
        }
        if (Test-Path -LiteralPath $ControlPath -PathType Leaf) {
            try {
                $control = Get-Content -LiteralPath $ControlPath -Raw | ConvertFrom-Json -ErrorAction Stop
                break
            } catch {
                Start-Sleep -Milliseconds 50
            }
        }
        Start-Sleep -Milliseconds 50
    }
    if ($null -eq $control -or -not $control.ok -or [int]$control.port -le 0) {
        throw 'Image-cache mock did not publish a loopback port.'
    }
    $baseUrl = "http://127.0.0.1:$([int]$control.port)"
    $health = Invoke-RestMethod -Uri "$baseUrl/__health" -TimeoutSec 3
    if (-not $health.ok) { throw 'Image-cache mock health check failed.' }

    $devProcesses = @(
        Get-Process -Name foobar2000 -ErrorAction SilentlyContinue |
            Where-Object { $_.Path -eq (Resolve-Path -LiteralPath $Exe).Path }
    )
    if ($devProcesses.Count -gt 0 -and -not $Deploy) {
        throw 'The dev instance is already running; rerun with -Deploy so the startup marker is consumed.'
    }

    $marker = [ordered]@{
        probe = 'image-cache'
        runId = $RunId
        baseUrl = $baseUrl
    } | ConvertTo-Json -Compress
    [IO.File]::WriteAllText($RequestPath, $marker, [Text.UTF8Encoding]::new($false))

    if ($Deploy) {
        & (Join-Path $PSScriptRoot 'run-dev.ps1') -Configuration Release -Kill
    } else {
        Start-Process -FilePath $Exe -WorkingDirectory $Instance -WindowStyle Hidden | Out-Null
    }
    $startedDevInstance = $true

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $probeResult = $null
    while ([DateTime]::UtcNow -lt $deadline) {
        if (Test-Path -LiteralPath $ProbeResultPath -PathType Leaf) {
            try {
                $probeResult = Get-Content -LiteralPath $ProbeResultPath -Raw |
                    ConvertFrom-Json -ErrorAction Stop
                break
            } catch {
                Start-Sleep -Milliseconds 100
            }
        }
        Start-Sleep -Milliseconds 100
    }
    if ($null -eq $probeResult) {
        throw "Image-cache probe did not finish within $TimeoutSeconds seconds."
    }

    $mockCounters = Invoke-RestMethod -Uri "$baseUrl/__counters" -TimeoutSec 5
    $probeResult | Add-Member -NotePropertyName mockCounters -NotePropertyValue $mockCounters -Force
    $probeResult | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $FinalResultPath -Encoding utf8NoBOM
    $mockCounters | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $CountersPath -Encoding utf8NoBOM

    $requiredCases = @(
        'confirmed-absence', 'cached-first-304', 'changed-200', 'mislabelled-mime', 'bad-html', 'truncated',
        'no-store', 'coalesce', 'invalidate-inflight', 'rotation-late',
        'shutdown-final'
    )
    if ([int]$probeResult.caseCount -ne $requiredCases.Count) {
        throw 'Image-cache probe returned an unexpected case count.'
    }
    $caseRows = @{}
    foreach ($row in @($probeResult.cases)) {
        $caseRows[[string]$row.name] = $row
    }
    foreach ($name in $requiredCases) {
        if (-not $caseRows.ContainsKey($name)) {
            throw "Image-cache probe omitted case $name."
        }
        if (-not [bool]$caseRows[$name].ok) {
            $detail = [string]$caseRows[$name].detail
            throw "Image-cache case $name failed: $detail"
        }
    }
    if (-not [bool]$probeResult.ok -or @($probeResult.failureCodes).Count -ne 0) {
        throw 'Image-cache probe reported failure codes.'
    }

    $byCase = $mockCounters.byCase
    if ([int]$byCase.'cached-first'.conditionalRequests -lt 1 -or
        [int]$byCase.'cached-first'.notModified -lt 1) {
        throw 'Cached-first case did not issue a slow conditional GET ending in 304.'
    }
    if ([int]$byCase.changed.changedResponses -lt 1) {
        throw 'Changed-image case did not return a replacement body.'
    }
    if ([int]$byCase.'mislabelled-mime'.requests -ne 1) {
        throw 'Mislabelled PNG case did not fetch exactly once before its cache hit.'
    }
    if ([int]$byCase.'bad-html'.invalidResponses -lt 1 -or
        [int]$byCase.truncated.invalidResponses -lt 1) {
        throw 'Invalid image responses were not observed by the mock.'
    }
    if ([int]$byCase.'no-store'.noStoreResponses -lt 2) {
        throw 'no-store behavior did not exercise background validation and a later cold demand.'
    }
    if ([int]$byCase.coalesce.requests -ne 1) {
        throw "Concurrent same-key requests produced $($byCase.coalesce.requests) network requests."
    }
    if ([int]$byCase.invalidate.requests -lt 2) {
        throw 'Invalidate-in-flight did not start a replacement refresh without another UI demand.'
    }
    if ([int]$byCase.rotation.requests -lt 2) {
        throw 'Account rotation did not reach the fresh-scope request.'
    }
    foreach ($route in @('shutdown-finish-0', 'shutdown-finish-1', 'shutdown-finish-2')) {
        $counter = $byCase.PSObject.Properties[$route].Value
        if ($null -eq $counter) {
            throw "Shutdown finish/wait route $route was not observed."
        }
        if ([int]$counter.requests -ne 1) {
            throw "Shutdown finish/wait route $route produced $($counter.requests) requests instead of one."
        }
    }
    if ([int]$byCase.shutdown.requests -ne 1) {
        throw "Shutdown case produced $($byCase.shutdown.requests) requests instead of one coalesced request."
    }
    if ([int]$probeResult.eventCount -lt 2) {
        throw 'Expected changed-image events were not observed.'
    }

    Write-Host "Image-cache regression passed. Artifacts: $TestRoot"
    $probeResult.ok
} catch {
    Write-Host "Image-cache regression artifacts: $TestRoot"
    throw
} finally {
    if ($startedDevInstance) { Stop-DevInstance }
    Stop-MockServer
    Remove-Item -LiteralPath $RequestPath, $ClaimedPath, $ProbeResultPath -Force -ErrorAction SilentlyContinue
}
