#Requires -Version 7.2

Set-StrictMode -Version Latest

$script:RepoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
$script:AppleMusicRoot = Join-Path $script:RepoRoot '.local\applemusic'
$script:WindowsRoot = Join-Path $script:AppleMusicRoot 'windows'
$script:RuntimeRoot = Join-Path $script:WindowsRoot 'runtime'
$script:DepsRoot = Join-Path $script:WindowsRoot 'deps'
$script:CacheRoot = Join-Path $script:WindowsRoot 'cache'
$script:StageRoot = Join-Path $script:WindowsRoot 'stage\adam-nightly-headless'
$script:NightlyRoot = Join-Path $script:AppleMusicRoot 'adam-nightly'
$script:LibhybrisRoot = Join-Path $script:AppleMusicRoot 'libhybris-win'
$script:TokenPath = Join-Path $script:AppleMusicRoot 'api-token.txt'
$script:EnginePath = Join-Path $script:RuntimeRoot 'engine.exe'
$script:PinsPath = Join-Path $script:RepoRoot 'scripts\applemusic\windows\source-pins.json'
$script:RuntimeManifestPath = Join-Path $script:RuntimeRoot 'manifest.json'
$script:BridgeConfigPath = Join-Path $script:RepoRoot '.local\fb2k-dev\profile\glasswave\applemusic\bridge.json'
$script:DefaultPort = 20025

function Get-GlasswavePaths {
    [pscustomobject]@{
        RepoRoot = $script:RepoRoot
        AppleMusicRoot = $script:AppleMusicRoot
        WindowsRoot = $script:WindowsRoot
        RuntimeRoot = $script:RuntimeRoot
        DepsRoot = $script:DepsRoot
        CacheRoot = $script:CacheRoot
        StageRoot = $script:StageRoot
        NightlyRoot = $script:NightlyRoot
        LibhybrisRoot = $script:LibhybrisRoot
        TokenPath = $script:TokenPath
        EnginePath = $script:EnginePath
        PinsPath = $script:PinsPath
        RuntimeManifestPath = $script:RuntimeManifestPath
        BridgeConfigPath = $script:BridgeConfigPath
    }
}

function Get-GlasswaveRuntimeFiles {
    $paths = Get-GlasswavePaths
    return @(
        $paths.EnginePath
        (Join-Path $paths.RuntimeRoot 'config.yaml')
        (Join-Path $paths.RuntimeRoot 'libdrm_client.dll')
        (Join-Path $paths.RuntimeRoot 'libhybris-core.dll')
        (Join-Path $paths.RuntimeRoot 'hybris-linker\q.so')
        (Join-Path $paths.RuntimeRoot 'libgcc_s_seh-1.dll')
        (Join-Path $paths.RuntimeRoot 'libstdc++-6.dll')
        (Join-Path $paths.RuntimeRoot 'libwinpthread-1.dll')
        (Join-Path $paths.RuntimeRoot 'rootfs\system\lib64\libandroidappmusic.so')
        (Join-Path $paths.RuntimeRoot 'rootfs\system\lib64\libandroid.so')
        (Join-Path $paths.RuntimeRoot 'rootfs\system\lib64\libdl.so')
        (Join-Path $paths.RuntimeRoot 'rootfs\system\lib64\libOpenSLES.so')
    )
}

function Test-GlasswaveRuntimeComplete {
    foreach ($path in Get-GlasswaveRuntimeFiles) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            return $false
        }
    }
    return $true
}

function Assert-GlasswaveRuntimeComplete {
    $missing = @(
        Get-GlasswaveRuntimeFiles |
            Where-Object { -not (Test-Path -LiteralPath $_ -PathType Leaf) }
    )
    if ($missing.Count -ne 0) {
        throw "Native Apple Music runtime is incomplete: $($missing -join ', ')"
    }
}

function Set-GlasswavePrivateAcl {
    param(
        [Parameter(Mandatory)]
        [string]$Path
    )

    $item = Get-Item -LiteralPath $Path -Force
    $user = [Security.Principal.WindowsIdentity]::GetCurrent().User
    $system = [Security.Principal.SecurityIdentifier]::new('S-1-5-18')
    if ($item.PSIsContainer) {
        $acl = [Security.AccessControl.DirectorySecurity]::new()
        $acl.SetAccessRuleProtection($true, $false)
        foreach ($identity in @($user, $system)) {
            $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
                $identity, 'FullControl', 'ContainerInherit,ObjectInherit', 'None', 'Allow'))
        }
        [IO.FileSystemAclExtensions]::SetAccessControl([IO.DirectoryInfo]::new($item.FullName), $acl)
    }
    else {
        $acl = [Security.AccessControl.FileSecurity]::new()
        $acl.SetAccessRuleProtection($true, $false)
        foreach ($identity in @($user, $system)) {
            $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
                $identity, 'FullControl', 'None', 'None', 'Allow'))
        }
        [IO.FileSystemAclExtensions]::SetAccessControl([IO.FileInfo]::new($item.FullName), $acl)
    }
}

function Initialize-GlasswavePrivateRuntime {
    $paths = Get-GlasswavePaths
    foreach ($path in @(
        $paths.AppleMusicRoot,
        $paths.WindowsRoot,
        $paths.RuntimeRoot,
        (Join-Path $paths.RuntimeRoot 'files'),
        (Join-Path $paths.RuntimeRoot 'files\mpl_db'),
        (Join-Path $paths.RuntimeRoot 'widevine'),
        (Join-Path $paths.RuntimeRoot 'logs'),
        (Join-Path $paths.RuntimeRoot 'config'),
        (Join-Path $paths.RuntimeRoot 'localappdata'),
        (Join-Path $paths.RuntimeRoot 'tmp')
    )) {
        New-Item -ItemType Directory -Force -Path $path | Out-Null
        Set-GlasswavePrivateAcl $path
    }
}

function Initialize-GlasswaveApiToken {
    $paths = Get-GlasswavePaths
    if (Test-Path -LiteralPath $paths.TokenPath -PathType Leaf) {
        Read-GlasswaveApiToken | Out-Null
        return
    }

    $token = [Convert]::ToHexString(
        [Security.Cryptography.RandomNumberGenerator]::GetBytes(32)
    )
    try {
        New-Item -ItemType Directory -Force -Path (Split-Path $paths.TokenPath -Parent) | Out-Null
        [IO.File]::WriteAllText($paths.TokenPath, $token, [Text.UTF8Encoding]::new($false))
        Set-GlasswavePrivateAcl $paths.TokenPath
    }
    finally {
        $token = $null
    }
}

function Read-GlasswaveApiToken {
    $path = $script:TokenPath
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "API token file is missing: $path"
    }
    Set-GlasswavePrivateAcl $path
    $token = [IO.File]::ReadAllText($path).Trim()
    if ([string]::IsNullOrWhiteSpace($token) -or $token.Length -gt 4096 -or $token -match '\s') {
        throw 'API token is empty, too long, or contains whitespace.'
    }
    return $token
}

function Get-GlasswavePidPath {
    param([Parameter(Mandatory)][int]$Port)
    Join-Path $script:RuntimeRoot "engine-$Port.pid.json"
}

function Read-GlasswavePidRecord {
    param([Parameter(Mandatory)][int]$Port)

    $path = Get-GlasswavePidPath $Port
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        return $null
    }
    try {
        return Get-Content -LiteralPath $path -Raw | ConvertFrom-Json -ErrorAction Stop
    }
    catch {
        return $null
    }
}

function Test-GlasswaveNativeProcess {
    param(
        [Parameter(Mandatory)][int]$Port,
        [switch]$Quiet
    )

    $record = Read-GlasswavePidRecord $Port
    if ($null -eq $record) {
        if (-not $Quiet) { Write-Host 'Native Apple Music helper: stopped' }
        return $false
    }
    if ([int]$record.port -ne $Port) {
        if (-not $Quiet) { Write-Host 'Native Apple Music helper: stale record (port mismatch)' }
        return $false
    }

    $process = Get-Process -Id ([int]$record.enginePid) -ErrorAction SilentlyContinue
    if ($null -eq $process -or $process.HasExited) {
        if (-not $Quiet) { Write-Host 'Native Apple Music helper: stale record (process exited)' }
        return $false
    }

    $expected = [IO.Path]::GetFullPath($script:EnginePath)
    $actual = if ($process.Path) { [IO.Path]::GetFullPath($process.Path) } else { '' }
    if (-not [string]::Equals($actual, $expected, [StringComparison]::OrdinalIgnoreCase)) {
        if (-not $Quiet) { Write-Host 'Native Apple Music helper: PID belongs to another executable' }
        return $false
    }

    $recordStart = $record.startedUtc.ToUniversalTime()
    $actualStart = $process.StartTime.ToUniversalTime()
    if ([math]::Abs(($actualStart - $recordStart).TotalSeconds) -gt 2) {
        if (-not $Quiet) { Write-Host 'Native Apple Music helper: stale record (start time mismatch)' }
        return $false
    }

    try {
        $cim = Get-CimInstance Win32_Process -Filter "ProcessId = $($process.Id)" -ErrorAction Stop
        if ($null -ne $cim -and $cim.CommandLine -notmatch "--api\s+$Port(?:\s|$)") {
            if (-not $Quiet) { Write-Host 'Native Apple Music helper: command line does not own this port' }
            return $false
        }
    }
    catch {
        if (-not $Quiet) { Write-Host 'Native Apple Music helper: command-line ownership is unverified' }
        return $false
    }
    return $true
}

function Test-GlasswavePortListening {
    param([Parameter(Mandatory)][int]$Port)
    $client = [Net.Sockets.TcpClient]::new()
    try {
        return $client.ConnectAsync('127.0.0.1', $Port).Wait(500)
    }
    catch {
        return $false
    }
    finally {
        $client.Dispose()
    }
}

function Invoke-GlasswaveApi {
    param(
        [Parameter(Mandatory)][int]$Port,
        [Parameter(Mandatory)][string]$Path,
        [string]$Method = 'GET',
        [string]$Body,
        [int]$TimeoutSeconds = 30
    )

    $token = Read-GlasswaveApiToken
    $handler = [Net.Http.HttpClientHandler]::new()
    $handler.UseProxy = $false
    $client = [Net.Http.HttpClient]::new($handler)
    $client.Timeout = [TimeSpan]::FromSeconds($TimeoutSeconds)
    try {
        $request = [Net.Http.HttpRequestMessage]::new([Net.Http.HttpMethod]::new($Method), "http://127.0.0.1:$Port$Path")
        $request.Headers.Add('X-Api-Token', $token)
        if (-not [string]::IsNullOrEmpty($Body)) {
            $request.Content = [Net.Http.StringContent]::new($Body, [Text.Encoding]::UTF8, 'application/json')
        }
        $response = $client.SendAsync($request).GetAwaiter().GetResult()
        $content = $response.Content.ReadAsStringAsync().GetAwaiter().GetResult()
        return [pscustomobject]@{
            StatusCode = [int]$response.StatusCode
            Body = $content
        }
    }
    finally {
        $client.Dispose()
        $token = $null
    }
}

function Wait-GlasswaveNativeReady {
    param(
        [Parameter(Mandatory)][int]$Port,
        [int]$TimeoutSeconds = 45
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $record = Read-GlasswavePidRecord $Port
        $process = if ($null -ne $record) {
            Get-Process -Id ([int]$record.enginePid) -ErrorAction SilentlyContinue
        } else {
            $null
        }
        if ($null -eq $process -or $process.HasExited) {
            return $false
        }
        try {
            $response = Invoke-GlasswaveApi -Port $Port -Path '/api/v1/status' -TimeoutSeconds 3
            if ($response.StatusCode -eq 200) {
                return $true
            }
        }
        catch {
        }
        Start-Sleep -Milliseconds 250
    }
    return $false
}

function Protect-GlasswaveLogLine {
    param(
        [AllowNull()][string]$Line,
        [AllowNull()][string]$Token
    )
    if ($null -eq $Line) {
        return $null
    }
    $safe = $Line
    if (-not [string]::IsNullOrEmpty($Token)) {
        $safe = $safe.Replace($Token, '[redacted-token]')
    }
    $safe = [regex]::Replace($safe, 'eyJ[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+', '[redacted-jwt]')
    $safe = [regex]::Replace($safe, '(?i)(authorization:\s*bearer\s+)\S+', '$1[redacted]')
    $safe = [regex]::Replace($safe, '(?i)(x-api-token\s*[:=]\s*)\S+', '$1[redacted]')
    return $safe
}

function Get-GlasswaveSourcePins {
    if (-not (Test-Path -LiteralPath $script:PinsPath -PathType Leaf)) {
        throw "Apple Music source pin manifest is missing: $script:PinsPath"
    }
    return Get-Content -LiteralPath $script:PinsPath -Raw | ConvertFrom-Json -ErrorAction Stop
}

function Assert-GlasswavePinnedRepository {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$ExpectedCommit
    )

    if (-not (Test-Path -LiteralPath (Join-Path $Path '.git'))) {
        throw "Pinned Git checkout is missing: $Path"
    }
    $actual = (& git -C $Path rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to read the Git commit for: $Path"
    }
    if (-not [string]::Equals($actual, $ExpectedCommit, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Pinned checkout mismatch at $Path (expected $ExpectedCommit, found $actual)."
    }
    return $actual
}

function Write-GlasswaveJsonFile {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)]$Value
    )

    $directory = Split-Path -Parent $Path
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $temporary = "$Path.tmp.$PID"
    try {
        $json = $Value | ConvertTo-Json -Depth 12
        [IO.File]::WriteAllText($temporary, $json, [Text.UTF8Encoding]::new($false))
        Move-Item -LiteralPath $temporary -Destination $Path -Force
        Set-GlasswavePrivateAcl $Path
    }
    finally {
        if (Test-Path -LiteralPath $temporary) {
            Remove-Item -LiteralPath $temporary -Force
        }
    }
}

function Remove-GlasswavePidRecord {
    param([Parameter(Mandatory)][int]$Port)

    $path = Get-GlasswavePidPath $Port
    $runtime = [IO.Path]::GetFullPath($script:RuntimeRoot).TrimEnd('\') + '\'
    $resolved = [IO.Path]::GetFullPath($path)
    if (-not $resolved.StartsWith($runtime, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove a PID record outside the private runtime: $resolved"
    }
    if (Test-Path -LiteralPath $resolved) {
        Remove-Item -LiteralPath $resolved -Force
    }
}

function Stop-GlasswaveNativeProcess {
    param(
        [Parameter(Mandatory)][int]$Port,
        [ValidateRange(1, 30)][int]$TimeoutSeconds = 8
    )

    $record = Read-GlasswavePidRecord $Port
    if ($null -eq $record) {
        Write-Host "Native Apple Music helper is not running on 127.0.0.1:$Port"
        return $false
    }
    if (-not (Test-GlasswaveNativeProcess -Port $Port -Quiet)) {
        Remove-GlasswavePidRecord $Port
        Write-Host "Removed stale native helper ownership record for port $Port"
        return $false
    }

    $processId = [int]$record.enginePid
    $process = Get-Process -Id $processId -ErrorAction Stop
    $expectedStart = $process.StartTime.ToUniversalTime()
    Stop-Process -Id $processId
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $current = Get-Process -Id $processId -ErrorAction SilentlyContinue
        if ($null -eq $current -or $current.HasExited) {
            Remove-GlasswavePidRecord $Port
            Write-Host "Native Apple Music helper stopped on 127.0.0.1:$Port"
            return $true
        }
        Start-Sleep -Milliseconds 200
    }

    $current = Get-Process -Id $processId -ErrorAction SilentlyContinue
    if ($null -ne $current -and
        [math]::Abs(($current.StartTime.ToUniversalTime() - $expectedStart).TotalSeconds) -le 2) {
        Stop-Process -Id $processId -Force
    }
    Remove-GlasswavePidRecord $Port
    Write-Host "Native Apple Music helper stopped on 127.0.0.1:$Port"
    return $true
}

function Write-GlasswaveBridgeConfig {
    param([ValidateRange(1, 65535)][int]$Port = $script:DefaultPort)

    $token = Read-GlasswaveApiToken
    try {
        if (-not [IO.Path]::IsPathFullyQualified($script:RuntimeRoot)) {
            throw "Managed Apple Music runtime directory must be absolute: $script:RuntimeRoot"
        }
        Write-GlasswaveJsonFile -Path $script:BridgeConfigPath -Value ([ordered]@{
            token = $token
            url = "http://127.0.0.1:$Port"
            managed = [ordered]@{
                runtimeDirectory = [IO.Path]::GetFullPath($script:RuntimeRoot)
            }
        })
    }
    finally {
        $token = $null
    }
}

function Test-GlasswaveBridgeConfig {
    param([ValidateRange(1, 65535)][int]$Port = $script:DefaultPort)

    if (-not (Test-Path -LiteralPath $script:BridgeConfigPath -PathType Leaf)) {
        return $false
    }
    try {
        $existing = Get-Content -LiteralPath $script:BridgeConfigPath -Raw | ConvertFrom-Json -ErrorAction Stop
        $token = Read-GlasswaveApiToken
        try {
            $runtimeDirectory = [string]$existing.managed.runtimeDirectory
            return $existing.url -eq "http://127.0.0.1:$Port" -and
                [string]::Equals([string]$existing.token, $token, [StringComparison]::Ordinal) -and
                [IO.Path]::IsPathFullyQualified($runtimeDirectory) -and
                [string]::Equals(
                    [IO.Path]::GetFullPath($runtimeDirectory),
                    [IO.Path]::GetFullPath($script:RuntimeRoot),
                    [StringComparison]::OrdinalIgnoreCase)
        }
        finally {
            $token = $null
        }
    }
    catch {
        return $false
    }
}

function Write-GlasswaveRuntimeManifest {
    $paths = Get-GlasswavePaths
    Write-GlasswaveJsonFile -Path $paths.RuntimeManifestPath -Value ([ordered]@{
        schema = 2
        platform = 'windows-amd64'
        generatedUtc = [DateTime]::UtcNow.ToString('o')
    })
}
