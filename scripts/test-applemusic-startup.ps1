#Requires -Version 7.2
<#
.SYNOPSIS
    Exercise Apple Music account verification and warm-cache startup timing.

.DESCRIPTION
    Uses only the repository development instance and a token-protected loopback mock.
    The native probe is model-level: it does not automate the UI, screenshots or playback.
    Cached navigation and list prefixes must be visible before account completion. A known
    playlist page must start after auth without waiting for global membership or DRM. A
    changed accountId must revoke the prior account at auth, before membership completes.
#>
[CmdletBinding()]
param(
    [ValidateRange(10, 180)]
    [int]$TimeoutSeconds = 60,

    [switch]$SeedOnly,

    [switch]$SameOnly
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Instance = Join-Path $Root '.local/fb2k-dev'
$Exe = Join-Path $Instance 'foobar2000.exe'
$ProbeDir = Join-Path $Instance 'profile/glasswave/applemusic'
$BridgePath = Join-Path $ProbeDir 'bridge.json'
$RequestPath = Join-Path $ProbeDir 'provider-test-request.json'
$ResultPath = Join-Path $ProbeDir 'provider-test-result.json'
$TestRoot = Join-Path $Root '.local/tests'
$EvidencePath = Join-Path $TestRoot 'applemusic-startup-contract.json'
$SeedDiagnosticPath = Join-Path $TestRoot 'finalNov11-unified-provider-r5-startup-seed-diagnostic.json'

if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) {
    throw 'Dev instance missing; bootstrap is out of scope for this test.'
}

function Stop-DevInstance {
    param([switch]$AllowForce)

    $exePath = (Resolve-Path -LiteralPath $Exe).Path
    $processes = @(
        Get-Process -Name foobar2000 -ErrorAction SilentlyContinue |
            Where-Object {
                try { $_.Path -eq $exePath }
                catch { $false }
            }
    )
    foreach ($process in $processes) {
        [void]$process.CloseMainWindow()
        if (-not $process.WaitForExit(8000)) {
            if (-not $AllowForce) {
                throw "Verified dev player PID $($process.Id) did not stop gracefully."
            }
            $process.Kill()
            [void]$process.WaitForExit(3000)
        }
    }
}

function Wait-MockFiles {
    param(
        [System.Diagnostics.Process]$Process,
        [string]$StateFile,
        [string]$BridgeFile
    )

    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    while (-not (Test-Path -LiteralPath $StateFile -PathType Leaf) -or
           -not (Test-Path -LiteralPath $BridgeFile -PathType Leaf)) {
        $Process.Refresh()
        if ($Process.HasExited -or [DateTime]::UtcNow -ge $deadline) {
            throw 'The startup provider mock did not start.'
        }
        Start-Sleep -Milliseconds 100
    }
}

function Start-ProviderMock {
    param(
        [string]$Suffix,
        [string]$Nonce
    )

    $stateFile = Join-Path $TestRoot "applemusic-startup-$PID-$Suffix.state.json"
    $bridgeFile = Join-Path $TestRoot "applemusic-startup-$PID-$Suffix.bridge.json"
    $errorFile = Join-Path $TestRoot "applemusic-startup-$PID-$Suffix.error.log"
    $mockScript = Join-Path $Root 'tests/applemusic_provider_mock.mjs'
    $arguments = @(
        ('"' + $mockScript + '"'),
        '--state-file', ('"' + $stateFile + '"'),
        '--bridge-file', ('"' + $bridgeFile + '"'),
        '--nonce', $Nonce,
        '--scenario', 'startup-cache'
    )
    $process = Start-Process -FilePath $script:Node -ArgumentList $arguments `
        -WorkingDirectory $Root -WindowStyle Hidden -PassThru `
        -RedirectStandardError $errorFile
    Wait-MockFiles -Process $process -StateFile $stateFile -BridgeFile $bridgeFile
    return [pscustomobject]@{
        Process = $process
        StateFile = $stateFile
        BridgeFile = $bridgeFile
        ErrorFile = $errorFile
    }
}

function Get-MockState {
    param([pscustomobject]$Mock)
    return Get-Content -LiteralPath $Mock.StateFile -Raw | ConvertFrom-Json -ErrorAction Stop
}

function Set-StartupMode {
    param(
        [pscustomobject]$Mock,
        [ValidateSet('seed', 'verify-slow', 'account-b', 'signed-out', 'transient')]
        [string]$Mode
    )

    $bridge = Get-Content -LiteralPath $Mock.BridgeFile -Raw | ConvertFrom-Json -ErrorAction Stop
    $headers = @{ 'X-API-Token' = [string]$bridge.token }
    $uri = "$($bridge.url)/api/v1/glasswave/testing/startup-mode/$Mode"
    $response = Invoke-RestMethod -Method Post -Uri $uri -Headers $headers -NoProxy `
        -TimeoutSec 10 -ErrorAction Stop
    if (-not $response.ok -or [string]$response.mode -ne $Mode) {
        throw "The startup mock did not enter mode $Mode."
    }
}

function Copy-MockBridge {
    param([pscustomobject]$Mock)
    Copy-Item -LiteralPath $Mock.BridgeFile -Destination $BridgePath -Force
}

function Invoke-ProviderProbe {
    param(
        [string]$Scenario,
        [string]$Nonce
    )

    Remove-Item -LiteralPath $ResultPath -Force -ErrorAction SilentlyContinue
    [IO.File]::WriteAllText(
        $RequestPath,
        (@{ nonce = $Nonce; scenario = $Scenario } | ConvertTo-Json -Compress),
        [Text.UTF8Encoding]::new($false)
    )
    $player = Start-Process -FilePath $Exe -WorkingDirectory $Instance `
        -ArgumentList '/stop' -WindowStyle Hidden -PassThru
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $result = $null
    while ([DateTime]::UtcNow -lt $deadline) {
        if (Test-Path -LiteralPath $ResultPath -PathType Leaf) {
            try {
                $result = Get-Content -LiteralPath $ResultPath -Raw |
                    ConvertFrom-Json -ErrorAction Stop
                break
            }
            catch {
                Start-Sleep -Milliseconds 100
            }
        }
        $player.Refresh()
        if ($player.HasExited) {
            throw "The dev player exited before startup probe $Scenario completed."
        }
        Start-Sleep -Milliseconds 100
    }
    if ($null -eq $result) {
        throw "Startup probe $Scenario did not finish within $TimeoutSeconds seconds."
    }
    if (-not $result.ok) {
        throw "Startup probe $Scenario failed: $($result.error)."
    }
    return $result
}

function Require-Operation {
    param(
        [pscustomobject]$Result,
        [string[]]$Names
    )

    $operations = @($Result.operations)
    foreach ($name in $Names) {
        $operation = $operations | Where-Object name -eq $name | Select-Object -First 1
        if ($null -eq $operation -or -not $operation.ok) {
            $detail = if ($null -eq $operation) { 'operation missing' } else { $operation.error }
            throw "Startup operation failed: $name ($detail)."
        }
    }
    return $operations
}

function Wait-MembershipComplete {
    param([pscustomobject]$Mock)

    $deadline = [DateTime]::UtcNow.AddSeconds(8)
    while ([DateTime]::UtcNow -lt $deadline) {
        $state = Get-MockState $Mock
        if ($state.startup.playlistCollectionCompleted -and
            $state.startup.albumCollectionCompleted -and
            $state.startup.artistCollectionCompleted -and
            $state.startup.drmCompleted) {
            return
        }
        Start-Sleep -Milliseconds 100
    }
    throw 'The startup mock membership/DRM phase did not finish.'
}

$savedBridge = $false
$savedBridgeBytes = $null
$savedBridgeAcl = $null
$bridgeReplaced = $false
$mocks = [Collections.Generic.List[object]]::new()
$testFailure = $null
$seedMockEvidence = $null
$nonce = [Convert]::ToHexString(
    [Security.Cryptography.RandomNumberGenerator]::GetBytes(32)
).ToLowerInvariant()
$evidence = [ordered]@{}

try {
    New-Item -ItemType Directory -Force -Path $TestRoot | Out-Null
    $script:Node = (& mise which node).Trim()
    if ($LASTEXITCODE -ne 0 -or -not $script:Node) {
        throw 'The project Node runtime is unavailable.'
    }

    Stop-DevInstance -AllowForce
    $savedBridge = Test-Path -LiteralPath $BridgePath -PathType Leaf
    if ($savedBridge) {
        $savedBridgeBytes = [IO.File]::ReadAllBytes($BridgePath)
        $savedBridgeAcl = [IO.FileSystemAclExtensions]::GetAccessControl(
            [IO.FileInfo]::new($BridgePath),
            [Security.AccessControl.AccessControlSections]::Access)
    }
    New-Item -ItemType Directory -Force -Path $ProbeDir | Out-Null

    $mockA = Start-ProviderMock -Suffix 'a' -Nonce $nonce
    $mocks.Add($mockA)
    $bridgeReplaced = $true
    Copy-MockBridge $mockA

    Set-StartupMode $mockA 'seed'
    $seed = Invoke-ProviderProbe -Scenario 'startup-cache-seed' -Nonce $nonce
    $seedOperations = Require-Operation $seed @('startup_seed_list', 'startup_seed_favorite')
    $evidence.seed = $seed
    if ($seedOperations.Count -ne 2) {
        throw "Startup seed expected 2 operations, received $($seedOperations.Count)."
    }
    $seedState = Get-MockState $mockA
    $seedMockEvidence = [ordered]@{
        scenario = $seedState.scenario
        errors = @($seedState.errors)
        unexpectedCalls = @($seedState.unexpectedCalls)
        startup = $seedState.startup
        catalog = $seedState.catalog
        writes = $seedState.writes
    }
    $evidence.seedMock = $seedMockEvidence
    [IO.File]::WriteAllText(
        $SeedDiagnosticPath,
        ($seedMockEvidence | ConvertTo-Json -Depth 20),
        [Text.UTF8Encoding]::new($false)
    )
    if ([int]$seedState.startup.invalidDisplaySongRequests -ne 0 -or
        [int]$seedState.startup.displaySongMissingCounterparts -lt 1) {
        throw 'The startup display-song contract was not exercised exactly.'
    }
    if (@($seedState.errors).Count -ne 0 -or @($seedState.unexpectedCalls).Count -ne 0) {
        throw 'The startup seed mock recorded errors or unexpected calls.'
    }
    if ($SeedOnly) {
        Write-Host 'Startup seed-only diagnostic: PASS'
        return
    }
    Stop-DevInstance

    Set-StartupMode $mockA 'verify-slow'
    $same = Invoke-ProviderProbe -Scenario 'startup-cache-same' -Nonce $nonce
    Require-Operation $same @(
        'startup_cached_before_auth',
        'startup_origin_bound',
        'startup_auth_before_membership',
        'startup_playlist_page_after_auth',
        'startup_favorite_warm_page_after_auth',
        'startup_same_identity_preserved'
    ) | Out-Null
    if (-not $same.startup.cachedSidebarBeforeAuth -or
        -not $same.startup.cachedListBeforeAuth -or
        -not $same.startup.restoringBeforeAuth -or
        -not $same.startup.accountUnverifiedBeforeAuth -or
        -not $same.startup.authPublishedBeforeMembership -or
        -not $same.startup.playlistPageAfterAuthBeforeMembership -or
        -not $same.startup.favoriteCachedBeforeAuth -or
        -not $same.startup.favoriteWarmPageAfterAuth -or
        -not $same.startup.sameQueryIdentity -or
        -not $same.startup.sameAccountPrefixPreserved -or
        [int]$same.startup.cachedRows -le 0 -or
        [int]$same.startup.listRequestsBeforeAuth -ne 0 -or
        [int]$same.startup.enrichmentRequestsBeforeAuth -ne 0) {
        throw 'Same-account warm-cache startup timing/state validation failed.'
    }
    $sameState = Get-MockState $mockA
    if ([int]$sameState.startup.playlistPageAfterAuthBeforeMembership -le 0 -or
        [int]$sameState.startup.favoritePageAfterAuthBeforeMembership -le 0) {
        throw 'Known playlist or Favorites first page waited for global membership or DRM.'
    }
    $evidence.same = $same
    $evidence.sameMock = $sameState.startup
    if ($SameOnly) {
        Write-Host 'Startup seed and same-account restart diagnostic: PASS'
        return
    }
    Wait-MembershipComplete $mockA
    Stop-DevInstance

    Set-StartupMode $mockA 'account-b'
    $accountB = Invoke-ProviderProbe -Scenario 'startup-cache-b' -Nonce $nonce
    Require-Operation $accountB @(
        'startup_cached_before_auth',
        'startup_origin_bound',
        'startup_auth_before_membership',
        'startup_b_revokes_a_at_auth',
        'startup_favorite_binding_cleared',
        'startup_favorite_no_favorite_empty'
    ) | Out-Null
    if (-not $accountB.startup.authPublishedBeforeMembership -or
        -not $accountB.startup.differentAccountRevokedA -or
        -not $accountB.startup.favoriteBindingClearedOnAccountChange -or
        -not $accountB.startup.favoriteNoFavoriteEmptyAfterDiscovery) {
        throw 'Changed accountId did not clear the A binding or publish confirmed Favorites empty.'
    }
    $accountBState = Get-MockState $mockA
    if ([string]::IsNullOrWhiteSpace([string]$seedState.account.accountId) -or
        [string]::IsNullOrWhiteSpace([string]$accountBState.account.accountId) -or
        [string]$seedState.account.accountId -eq [string]$accountBState.account.accountId -or
        [string]$seedState.account.name -ne [string]$accountBState.account.name -or
        [string]$seedState.account.storefront -ne [string]$accountBState.account.storefront) {
        throw 'Account A/B fixture must share display name/country but use different stable ids.'
    }
    $evidence.accountB = $accountB
    $evidence.accountBMock = $accountBState.startup
    Stop-DevInstance

    Set-StartupMode $mockA 'signed-out'
    $signedOut = Invoke-ProviderProbe -Scenario 'startup-cache-signed-out' -Nonce $nonce
    Require-Operation $signedOut @(
        'startup_cached_before_auth',
        'startup_origin_bound',
        'startup_auth_before_membership',
        'startup_signedout_clears'
    ) | Out-Null
    if (-not $signedOut.startup.signedOutCleared) {
        throw 'A signed-out startup retained private cached navigation.'
    }
    $evidence.signedOut = $signedOut
    Stop-DevInstance

    Set-StartupMode $mockA 'transient'
    $transient = Invoke-ProviderProbe -Scenario 'startup-cache-transient' -Nonce $nonce
    Require-Operation $transient @(
        'startup_cached_before_auth',
        'startup_origin_bound',
        'startup_transient_preserves'
    ) | Out-Null
    if (-not $transient.startup.transientPreserved) {
        throw 'A transient account 503 lost cached navigation or granted access.'
    }
    $transientState = Get-MockState $mockA
    $transientWrites = @(
        $transientState.writes.PSObject.Properties |
            Where-Object { [int]$_.Value -ne 0 }
    ) + @(
        $transientState.mutation.writes.PSObject.Properties |
            Where-Object { [int]$_.Value -ne 0 }
    )
    if ($transientWrites.Count -ne 0) {
        throw 'A transient account 503 granted a write-capable cache state.'
    }
    $evidence.transient = $transient
    $evidence.transientMock = $transientState.startup
    Stop-DevInstance

    $mockA.Process.Kill()
    [void]$mockA.Process.WaitForExit(3000)
    $mockB = Start-ProviderMock -Suffix 'b' -Nonce $nonce
    $mocks.Add($mockB)
    Copy-MockBridge $mockB
    Set-StartupMode $mockB 'verify-slow'
    $origin = Invoke-ProviderProbe -Scenario 'startup-cache-origin' -Nonce $nonce
    Require-Operation $origin @(
        'startup_origin_bound',
        'startup_origin_isolated',
        'startup_favorite_cold_pending',
        'startup_favorite_after_discovery'
    ) | Out-Null
    if (-not $origin.startup.originIsolation -or
        -not $origin.startup.favoriteColdPendingBeforeDiscovery -or
        -not $origin.startup.favoriteColdPageAfterDiscovery) {
        throw 'Cold Favorites target discovery or source-origin isolation failed.'
    }
    $evidence.origin = $origin
    $evidence.originMock = (Get-MockState $mockB).startup

    [IO.File]::WriteAllText(
        $EvidencePath,
        ([ordered]@{ failed = $false; phases = $evidence } | ConvertTo-Json -Depth 20),
        [Text.UTF8Encoding]::new($false)
    )
    Write-Host 'Startup auth/cache contract: seed and same-account restart complete'
    Write-Host 'Auth published before membership/DRM; known playlist page started immediately'
    Write-Host 'Changed accountId revoked A at auth; sign-out and transient failure validated'
    Write-Host 'Mock source origin did not load another origin''s cached navigation or list'
}
catch {
    $testFailure = $_.Exception.Message
    throw
}
finally {
    try { Stop-DevInstance -AllowForce } catch {}
    foreach ($mock in $mocks) {
        if ($null -ne $mock.Process -and -not $mock.Process.HasExited) {
            $mock.Process.Kill()
            [void]$mock.Process.WaitForExit(3000)
        }
    }
    try {
        [IO.File]::WriteAllText(
            $EvidencePath,
            ([ordered]@{
                failed = -not [string]::IsNullOrWhiteSpace($testFailure)
                testFailure = $testFailure
                phases = $evidence
            } | ConvertTo-Json -Depth 20),
            [Text.UTF8Encoding]::new($false)
        )
    }
    catch {}
    if (-not [string]::IsNullOrWhiteSpace($testFailure)) {
        try {
            $stamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
            $failureDir = Join-Path $TestRoot "applemusic-startup-failure-$stamp-$PID"
            New-Item -ItemType Directory -Force -Path $failureDir | Out-Null
            $artifacts = [ordered]@{}
            $candidates = @(
                [pscustomobject]@{ Name = 'request.json'; Path = $RequestPath },
                [pscustomobject]@{ Name = 'result.json'; Path = $ResultPath },
                [pscustomobject]@{ Name = 'bridge.json'; Path = $BridgePath },
                [pscustomobject]@{ Name = 'evidence.json'; Path = $EvidencePath },
                [pscustomobject]@{ Name = 'seed-diagnostic.json'; Path = $SeedDiagnosticPath }
            )
            for ($index = 0; $index -lt $mocks.Count; ++$index) {
                $mock = $mocks[$index]
                $candidates += @(
                    [pscustomobject]@{ Name = "mock-$index-state.json"; Path = $mock.StateFile },
                    [pscustomobject]@{ Name = "mock-$index-bridge.json"; Path = $mock.BridgeFile },
                    [pscustomobject]@{ Name = "mock-$index-error.log"; Path = $mock.ErrorFile }
                )
            }
            foreach ($item in $candidates) {
                if ([string]::IsNullOrWhiteSpace([string]$item.Path) -or
                    -not (Test-Path -LiteralPath $item.Path -PathType Leaf)) {
                    continue
                }
                $target = Join-Path $failureDir $item.Name
                Copy-Item -LiteralPath $item.Path -Destination $target -Force
                $artifacts[$item.Name] = $target
            }
            [IO.File]::WriteAllText(
                (Join-Path $failureDir 'failure.json'),
                ([ordered]@{
                    failed = $true
                    testFailure = $testFailure
                    phases = $evidence
                    artifacts = $artifacts
                } | ConvertTo-Json -Depth 20),
                [Text.UTF8Encoding]::new($false)
            )
            Write-Host "Startup failure artifacts: $failureDir"
        }
        catch {
            Write-Warning "Could not preserve startup failure artifacts: $($_.Exception.Message)"
        }
    }
    foreach ($mock in $mocks) {
        Remove-Item -LiteralPath $mock.StateFile, $mock.BridgeFile -Force -ErrorAction SilentlyContinue
    }
    if ($bridgeReplaced) {
        if ($savedBridge) {
            New-Item -ItemType Directory -Force -Path $ProbeDir | Out-Null
            [IO.File]::WriteAllBytes($BridgePath, $savedBridgeBytes)
            if ($savedBridgeAcl) {
                [IO.FileSystemAclExtensions]::SetAccessControl(
                    [IO.FileInfo]::new($BridgePath), $savedBridgeAcl)
            }
        }
        else {
            Remove-Item -LiteralPath $BridgePath -Force -ErrorAction SilentlyContinue
        }
    }
    Remove-Item -LiteralPath $RequestPath, $ResultPath -Force -ErrorAction SilentlyContinue
}
