#Requires -Version 7.2

[CmdletBinding()]
param(
    [switch]$CheckOnly,
    [switch]$RefreshStage,
    [switch]$SkipTests,
    [switch]$SkipFairPlayCheck,
    [switch]$PromoteRuntime,
    [switch]$PromoteArtifactOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'common.ps1')

$paths = Get-GlasswavePaths
$pins = Get-GlasswaveSourcePins
$stageRoot = Join-Path $paths.WindowsRoot 'stage'
$stageSource = Join-Path $stageRoot 'adam-nightly-headless'
$stageHybrisSource = Join-Path $stageRoot 'libhybris-win'
$stageOutput = Join-Path $stageRoot 'runtime-output'
$libhybrisOutput = Join-Path $paths.DepsRoot 'libhybris-win'
$libdrmOutput = Join-Path $paths.DepsRoot 'libdrm-client'
$stubOutput = Join-Path $paths.DepsRoot 'android-stubs'
$mingwBin = 'C:\ProgramData\mingw64\mingw64\bin'
$llvmBin = 'C:\Program Files\LLVM\bin'
$llvmNm = Join-Path $llvmBin 'llvm-nm.exe'

function Get-RequiredCommand {
    param([Parameter(Mandatory)][string]$Name)
    $command = Get-Command $Name -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $command) {
        throw "Required Windows build tool is missing: $Name"
    }
    return $command.Source
}

function Get-GitBashPath {
    $git = Get-RequiredCommand 'git.exe'
    $gitRoot = Split-Path (Split-Path $git -Parent) -Parent
    foreach ($candidate in @(
        (Join-Path $gitRoot 'bin\bash.exe'),
        (Join-Path $gitRoot 'usr\bin\bash.exe')
    )) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }
    throw 'Git for Windows bash.exe is required for the upstream libhybris build.'
}

function Get-VsEditBinPath {
    $vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw 'vswhere.exe is required to locate the Visual Studio PE editor.'
    }
    $candidates = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -find 'VC\Tools\MSVC\**\bin\Hostx64\x64\editbin.exe'
    if ($LASTEXITCODE -ne 0 -or @($candidates).Count -eq 0) {
        throw 'Visual Studio editbin.exe is required for the 2 MiB PE stack commit.'
    }
    return (@($candidates) | Sort-Object -Descending | Select-Object -First 1).Trim()
}

function Get-MiseGoPath {
    $mise = Get-RequiredCommand 'mise.exe'
    $resolved = & $mise exec "go@$($pins.buildTools.go)" -- where.exe go
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace(($resolved -join ''))) {
        throw "mise could not resolve Go $($pins.buildTools.go)."
    }
    return ($resolved | Select-Object -First 1).Trim()
}

function Assert-ToolPin {
    param(
        [Parameter(Mandatory)][string]$Command,
        [Parameter(Mandatory)][string]$ExpectedVersion
    )

    $version = if ([IO.Path]::GetFileName($Command) -ieq 'go.exe') {
        (& $Command version 2>&1 | Out-String)
    }
    else {
        (& $Command --version 2>&1 | Out-String)
    }
    if ($version -notmatch [regex]::Escape($ExpectedVersion)) {
        throw "Tool version mismatch for $Command (expected $ExpectedVersion). Actual: $($version.Trim())"
    }
}

function Ensure-PinnedCheckout {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Repository,
        [Parameter(Mandatory)][string]$Commit
    )

    if (-not (Test-Path -LiteralPath (Join-Path $Path '.git'))) {
        New-Item -ItemType Directory -Force -Path (Split-Path $Path -Parent) | Out-Null
        $savedLfs = $env:GIT_LFS_SKIP_SMUDGE
        try {
            $env:GIT_LFS_SKIP_SMUDGE = '1'
            & git -c core.autocrlf=false clone --filter=blob:none --no-checkout $Repository $Path
            if ($LASTEXITCODE -ne 0) {
                throw "Unable to clone pinned repository: $Repository"
            }
            & git -C $Path -c core.autocrlf=false checkout --detach $Commit
            if ($LASTEXITCODE -ne 0) {
                throw "Unable to check out pinned commit $Commit."
            }
        }
        finally {
            $env:GIT_LFS_SKIP_SMUDGE = $savedLfs
        }
    }
    return Assert-GlasswavePinnedRepository -Path $Path -ExpectedCommit $Commit
}

function Remove-GeneratedStageTree {
    param([Parameter(Mandatory)][string]$Path)

    $stageAbsolute = [IO.Path]::GetFullPath($stageRoot).TrimEnd('\') + '\'
    $resolved = [IO.Path]::GetFullPath($Path)
    if (-not $resolved.StartsWith($stageAbsolute, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove a generated stage outside $stageRoot`: $resolved"
    }
    if (Test-Path -LiteralPath $resolved) {
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}

function New-PinnedStage {
    param(
        [Parameter(Mandatory)][string]$Source,
        [Parameter(Mandatory)][string]$Stage,
        [Parameter(Mandatory)][string]$Commit,
        [Parameter(Mandatory)][string]$Patch
    )

    if (Test-Path -LiteralPath $Stage) {
        & git -C $Source worktree remove --force -- $Stage 2>$null
        if ($LASTEXITCODE -ne 0) {
            Remove-GeneratedStageTree $Stage
            & git -C $Source worktree prune
        }
    }
    & git -C $Source worktree add --detach --force $Stage $Commit
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to create isolated source stage at $Stage."
    }
    & git -C $Stage apply --3way --index $Patch
    if ($LASTEXITCODE -ne 0) {
        throw "Pinned patch did not apply cleanly: $Patch"
    }
}

function New-AndroidStubs {
    param(
        [Parameter(Mandatory)][string]$DrmSource,
        [Parameter(Mandatory)][string]$Output
    )

    $clang = Join-Path $llvmBin 'clang.exe'
    if (-not (Test-Path -LiteralPath $clang -PathType Leaf)) {
        throw "LLVM clang is missing: $clang"
    }
    New-Item -ItemType Directory -Force -Path $Output | Out-Null
    $stubSource = Join-Path $DrmSource 'stubs\empty_stub.c'
    $versionScript = Join-Path $DrmSource 'stubs\libdl.map'
    foreach ($name in @('libandroid.so', 'libOpenSLES.so', 'libdl.so')) {
        $arguments = @(
            '--target=x86_64-linux-gnu',
            '-fuse-ld=lld',
            '-shared',
            '-fPIC',
            '-nostdlib',
            '-O2',
            '-Wl,--hash-style=both',
            "-Wl,-soname,$name"
        )
        if ($name -eq 'libdl.so') {
            $arguments += "-Wl,--version-script=$versionScript"
        }
        $arguments += @('-o', (Join-Path $Output $name), $stubSource)
        & $clang @arguments
        if ($LASTEXITCODE -ne 0) {
            throw "Unable to generate Android ELF stub: $name"
        }
    }
}

function Assert-FairPlayLibrariesPresent {
    param([Parameter(Mandatory)][string]$LockPath)

    $root = Join-Path $paths.RuntimeRoot 'rootfs\system\lib64'
    $files = [Collections.Generic.List[string]]::new()
    foreach ($line in Get-Content -LiteralPath $LockPath) {
        if ($line -match '^\s*[0-9a-fA-F]{64}\s+([A-Za-z0-9._+-]+)\s*$') {
            $files.Add($Matches[1])
        }
    }
    if ($files.Count -eq 0) {
        throw "FairPlay lock contains no files: $LockPath"
    }
    foreach ($name in $files) {
        $path = Join-Path $root $name
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "FairPlay runtime file is missing: $path"
        }
    }
}

function Assert-FairPlayAccountDSIDSymbol {
    $library = Join-Path $paths.RuntimeRoot 'rootfs\system\lib64\libstoreservicescore.so'
    if (-not (Test-Path -LiteralPath $library -PathType Leaf)) {
        throw "Apple account library is missing: $library"
    }
    $symbol = '_ZN17storeservicescore14RequestContext20preferredAccountDSIDEv'
    $defined = & $llvmNm -D --defined-only $library
    if ($LASTEXITCODE -ne 0 -or
        -not (@($defined) | Where-Object { $_ -match "\s$([regex]::Escape($symbol))\s*$" })) {
        throw "Apple account library does not export the expected DSID getter: $symbol"
    }
}

function Assert-PeStackHeader {
    param([Parameter(Mandatory)][string]$Path)

    $llvmReadobj = Join-Path $llvmBin 'llvm-readobj.exe'
    if (-not (Test-Path -LiteralPath $llvmReadobj -PathType Leaf)) {
        throw "LLVM llvm-readobj is missing: $llvmReadobj"
    }
    $headers = & $llvmReadobj --file-header $Path | Out-String
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to read PE headers from $Path."
    }
    $expectedReserve = [long]$pins.windowsLink.stackReserveBytes
    $expectedCommit = [long]$pins.windowsLink.stackCommitBytes
    if ($headers -notmatch "(?m)^\s*SizeOfStackReserve:\s*([0-9]+)\s*$") {
        throw "PE stack reserve is missing in $Path."
    }
    $actualReserve = [long]$Matches[1]
    if ($headers -notmatch "(?m)^\s*SizeOfStackCommit:\s*([0-9]+)\s*$") {
        throw "PE stack commit is missing in $Path."
    }
    $actualCommit = [long]$Matches[1]
    if ($actualReserve -ne $expectedReserve -or $actualCommit -ne $expectedCommit) {
        throw "PE stack header mismatch in $Path (expected reserve $expectedReserve, commit $expectedCommit)."
    }
}

function Sync-FairPlayLibraries {
    param([Parameter(Mandatory)][string]$LockPath)

    $root = Join-Path $paths.RuntimeRoot 'rootfs\system\lib64'
    New-Item -ItemType Directory -Force -Path $root | Out-Null
    Set-GlasswavePrivateAcl $root
    $sources = [Collections.Generic.List[string]]::new()
    $files = [Collections.Generic.List[string]]::new()
    foreach ($line in Get-Content -LiteralPath $LockPath) {
        if ($line -match '^\s*source\s+(https://\S+)\s*$') {
            $sources.Add($Matches[1].TrimEnd('/'))
        }
        elseif ($line -match '^\s*[0-9a-fA-F]{64}\s+([A-Za-z0-9._+-]+)\s*$') {
            $files.Add($Matches[1])
        }
    }
    if ($sources.Count -eq 0 -or $files.Count -eq 0) {
        throw "FairPlay lock contains no usable sources or files: $LockPath"
    }

    foreach ($name in $files) {
        $destination = Join-Path $root $name
        if (Test-Path -LiteralPath $destination -PathType Leaf) {
            continue
        }

        $temporary = "$destination.download.$PID"
        $downloaded = $false
        try {
            foreach ($source in $sources) {
                try {
                    Invoke-WebRequest -Uri "$source/$name" -OutFile $temporary -UseBasicParsing -TimeoutSec 90
                    Move-Item -LiteralPath $temporary -Destination $destination -Force
                    Set-GlasswavePrivateAcl $destination
                    $downloaded = $true
                    break
                }
                catch {
                }
                if (Test-Path -LiteralPath $temporary) {
                    Remove-Item -LiteralPath $temporary -Force
                }
            }
            if (-not $downloaded) {
                throw "Unable to download pinned FairPlay runtime file: $name"
            }
        }
        finally {
            if (Test-Path -LiteralPath $temporary) {
                Remove-Item -LiteralPath $temporary -Force
            }
        }
    }
}

$go = Get-MiseGoPath
$gcc = Join-Path $mingwBin 'gcc.exe'
$gxx = Join-Path $mingwBin 'g++.exe'
$make = Join-Path $mingwBin 'mingw32-make.exe'
$gendef = Join-Path $mingwBin 'gendef.exe'
$dlltool = Join-Path $mingwBin 'dlltool.exe'
$objdump = Join-Path $mingwBin 'objdump.exe'
$clang = Join-Path $llvmBin 'clang.exe'
$gitBash = Get-GitBashPath
$editbin = Get-VsEditBinPath
foreach ($tool in @($go, $gcc, $gxx, $make, $gendef, $dlltool, $objdump, $llvmNm, $clang, $gitBash, $editbin)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
        throw "Required Windows build tool is missing: $tool"
    }
}
Assert-ToolPin -Command $go -ExpectedVersion $pins.buildTools.go
Assert-ToolPin -Command $gcc -ExpectedVersion $pins.buildTools.mingw
Assert-ToolPin -Command $clang -ExpectedVersion $pins.buildTools.llvm

$nightlyCommit = if ($CheckOnly) {
    Assert-GlasswavePinnedRepository -Path $paths.NightlyRoot -ExpectedCommit $pins.adam.commit
}
else {
    Ensure-PinnedCheckout `
        -Path $paths.NightlyRoot `
        -Repository $pins.adam.repository `
        -Commit $pins.adam.commit
}
$hybrisCommit = if ($CheckOnly) {
    Assert-GlasswavePinnedRepository -Path $paths.LibhybrisRoot -ExpectedCommit $pins.libhybrisWin.commit
}
else {
    Ensure-PinnedCheckout `
        -Path $paths.LibhybrisRoot `
        -Repository $pins.libhybrisWin.repository `
        -Commit $pins.libhybrisWin.commit
}

$fairplayLock = Join-Path $paths.NightlyRoot 'drm\fairplay-libs.lock'
if (-not $SkipFairPlayCheck) {
    if (-not $CheckOnly) {
        Sync-FairPlayLibraries $fairplayLock
    }
    Assert-FairPlayLibrariesPresent $fairplayLock
}

if ($CheckOnly -and $PromoteArtifactOnly) {
    throw 'CheckOnly and PromoteArtifactOnly cannot be combined.'
}

if ($CheckOnly) {
    Write-Host "Apple Music Windows build inputs: ready"
    Write-Host "Adam nightly: $nightlyCommit"
    Write-Host "libhybris-win: $hybrisCommit"
    Write-Host "Go: $go"
    Write-Host "MinGW: $gcc"
    Write-Host "LLVM: $clang"
    exit 0
}

if ($PromoteArtifactOnly) {
    $stageEngine = Join-Path $stageOutput 'engine.exe'
    foreach ($relative in @(
        'engine.exe',
        'libdrm_client.dll',
        'libhybris-core.dll',
        'hybris-linker\q.so',
        'libgcc_s_seh-1.dll',
        'libstdc++-6.dll',
        'libwinpthread-1.dll',
        'android-stubs\libandroid.so',
        'android-stubs\libdl.so',
        'android-stubs\libOpenSLES.so'
    )) {
        $artifactPath = Join-Path $stageOutput $relative
        if (-not (Test-Path -LiteralPath $artifactPath -PathType Leaf)) {
            throw "Isolated build artifact is missing: $artifactPath"
        }
    }
    Assert-PeStackHeader $stageEngine

    Initialize-GlasswavePrivateRuntime
    foreach ($name in @('engine.exe', 'libdrm_client.dll', 'libhybris-core.dll', 'hybris-linker\q.so')) {
        $source = Join-Path $stageOutput $name
        $destination = Join-Path $paths.RuntimeRoot $name
        New-Item -ItemType Directory -Force -Path (Split-Path $destination -Parent) | Out-Null
        Copy-Item -LiteralPath $source -Destination $destination -Force
    }
    $runtimeStubs = Join-Path $paths.RuntimeRoot 'android-stubs'
    New-Item -ItemType Directory -Force -Path $runtimeStubs | Out-Null
    Copy-Item -Path (Join-Path $stageOutput 'android-stubs\*') -Destination $runtimeStubs -Recurse -Force
    $runtimeLib64 = Join-Path $paths.RuntimeRoot 'rootfs\system\lib64'
    New-Item -ItemType Directory -Force -Path $runtimeLib64 | Out-Null
    foreach ($stub in @('libandroid.so', 'libdl.so', 'libOpenSLES.so')) {
        Copy-Item -LiteralPath (Join-Path $stageOutput "android-stubs\$stub") `
            -Destination (Join-Path $runtimeLib64 $stub) -Force
    }
    foreach ($dll in @('libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll')) {
        Copy-Item -LiteralPath (Join-Path $stageOutput $dll) `
            -Destination (Join-Path $paths.RuntimeRoot $dll) -Force
    }
    Write-GlasswaveRuntimeManifest
    Write-Host "Promoted isolated build: $(Join-Path $paths.RuntimeRoot 'engine.exe')"
    return
}

Assert-FairPlayAccountDSIDSymbol

if ($RefreshStage) {
    Remove-GeneratedStageTree $stageSource
    Remove-GeneratedStageTree $stageHybrisSource
    Remove-GeneratedStageTree $stageOutput
}
New-Item -ItemType Directory -Force -Path $stageRoot, $paths.RuntimeRoot, $libhybrisOutput, $libdrmOutput, $stubOutput, $stageOutput | Out-Null

New-PinnedStage `
    -Source $paths.NightlyRoot `
    -Stage $stageSource `
    -Commit $pins.adam.commit `
    -Patch (Join-Path $paths.RepoRoot $pins.adam.patch)

New-PinnedStage `
    -Source $paths.LibhybrisRoot `
    -Stage $stageHybrisSource `
    -Commit $pins.libhybrisWin.commit `
    -Patch (Join-Path $paths.RepoRoot $pins.libhybrisWin.patch)

New-AndroidStubs -DrmSource (Join-Path $stageSource 'drm') -Output $stubOutput

$savedPath = $env:PATH
$env:PATH = "$mingwBin;$libdrmOutput;$libhybrisOutput;$env:PATH"
try {
    $hybrisBuild = Join-Path $libhybrisOutput 'build'
    $hybrisLinker = Join-Path $libhybrisOutput 'hybris-linker'
    $env:HYBRIS_BUILD_DIR = $hybrisBuild -replace '\\', '/'
    $env:HYBRIS_LINKER_OUT = $hybrisLinker -replace '\\', '/'
    $env:HYBRIS_LINKER_DIR = $hybrisLinker -replace '\\', '/'
    $env:HYBRIS_SKIP_WINDOWS_SMOKE = '1'
    Push-Location $stageHybrisSource
    try {
        & $gitBash -c './build-and-test-x86_64.sh'
        if ($LASTEXITCODE -ne 0) {
            throw 'Upstream libhybris Windows build failed.'
        }
    }
    finally {
        Pop-Location
    }
    Copy-Item -LiteralPath (Join-Path $hybrisBuild 'libhybris-core.so') `
        -Destination (Join-Path $libhybrisOutput 'libhybris-core.dll') -Force
    $qSource = Join-Path $hybrisLinker 'q.so'
    $qDestination = Join-Path $libhybrisOutput 'hybris-linker\q.so'
    if (-not [string]::Equals(
        [IO.Path]::GetFullPath($qSource),
        [IO.Path]::GetFullPath($qDestination),
        [StringComparison]::OrdinalIgnoreCase)) {
        Copy-Item -LiteralPath $qSource -Destination $qDestination -Force
    }

    $drmBuild = Join-Path $libdrmOutput 'build'
    $env:MINGW_CC = $gcc -replace '\\', '/'
    $env:MINGW_CXX = $gxx -replace '\\', '/'
    $env:WINDOWS_BUILD_DIR = $drmBuild -replace '\\', '/'
    Push-Location (Join-Path $stageSource 'drm')
    try {
        & $gitBash -c 'mingw32-make windows-build'
        if ($LASTEXITCODE -ne 0) {
            throw 'Windows DRM client build failed.'
        }
    }
    finally {
        Pop-Location
    }
    Copy-Item -LiteralPath (Join-Path $drmBuild 'libdrm_client.dll') `
        -Destination (Join-Path $libdrmOutput 'libdrm_client.dll') -Force
    $drmDll = Join-Path $libdrmOutput 'libdrm_client.dll'
    $drmDef = Join-Path $libdrmOutput 'libdrm_client.def'
    $drmImportLibrary = Join-Path $libdrmOutput 'libdrm_client.dll.a'
    $defLines = @(& $gendef '--no-include-current-dir' '-' $drmDll)
    if ($LASTEXITCODE -ne 0) {
        throw 'gendef failed for libdrm_client.dll.'
    }
    [IO.File]::WriteAllText(
        $drmDef,
        (($defLines -join "`n") + "`n"),
        [Text.Encoding]::ASCII
    )
    & $dlltool '--deterministic-libraries' '-d' $drmDef '-l' $drmImportLibrary
    if ($LASTEXITCODE -ne 0) {
        throw 'dlltool failed for libdrm_client.dll.'
    }
    foreach ($rootArtifact in @(
        (Join-Path $paths.RepoRoot 'libdrm_client.def'),
        (Join-Path $paths.RepoRoot 'libdrm_client.dll.a')
    )) {
        if (Test-Path -LiteralPath $rootArtifact) {
            throw "DRM import generation wrote outside the private dependency root: $rootArtifact"
        }
    }

    Push-Location (Join-Path $stageSource 'engine')
    try {
        $env:CGO_ENABLED = '1'
        $env:GOOS = 'windows'
        $env:GOARCH = 'amd64'
        $env:CC = $gcc
        $env:CXX = $gxx
        $env:CGO_LDFLAGS = "-L$($libdrmOutput -replace '\\', '/') -L$($libhybrisOutput -replace '\\', '/')"
        if (-not $SkipTests) {
            & $go test -tags 'adamplay_backend glasswave' ./cmd ./core/apple ./core/playback `
                ./core/media ./core/fairplay ./core/hls ./utils/alacstream ./core/diskcache
            if ($LASTEXITCODE -ne 0) {
                throw 'Go tests failed.'
            }
        }
        & $go build `
            -tags 'adamplay_backend glasswave' `
            -trimpath `
            -buildvcs=false `
            -ldflags ([string]$pins.windowsLink.goExternalLdflags) `
            -o (Join-Path $stageOutput 'engine.exe') `
            ./cmd
        if ($LASTEXITCODE -ne 0) {
            throw 'Go native helper build failed.'
        }
        & $editbin /NOLOGO ([string]$pins.windowsLink.postLinkEditbinStack) (Join-Path $stageOutput 'engine.exe')
        if ($LASTEXITCODE -ne 0) {
            throw 'editbin failed to set the PE stack commit.'
        }
        Assert-PeStackHeader (Join-Path $stageOutput 'engine.exe')
    }
    finally {
        Pop-Location
        $env:CGO_ENABLED = $null
        $env:GOOS = $null
        $env:GOARCH = $null
        $env:CC = $null
        $env:CXX = $null
        $env:CGO_LDFLAGS = $null
    }
}
finally {
    $env:PATH = $savedPath
    $env:HYBRIS_BUILD_DIR = $null
    $env:HYBRIS_LINKER_OUT = $null
    $env:HYBRIS_LINKER_DIR = $null
    $env:HYBRIS_SKIP_WINDOWS_SMOKE = $null
    $env:MINGW_CC = $null
    $env:MINGW_CXX = $null
    $env:WINDOWS_BUILD_DIR = $null
}

foreach ($name in @('libdrm_client.dll', 'libhybris-core.dll', 'hybris-linker\q.so')) {
    $source = if ($name -eq 'libdrm_client.dll') {
        Join-Path $libdrmOutput $name
    }
    else {
        Join-Path $libhybrisOutput $name
    }
    $destination = Join-Path $stageOutput $name
    New-Item -ItemType Directory -Force -Path (Split-Path $destination -Parent) | Out-Null
    Copy-Item -LiteralPath $source -Destination $destination -Force
}
Copy-Item -LiteralPath $stubOutput -Destination (Join-Path $stageOutput 'android-stubs') -Recurse -Force
foreach ($dll in @('libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll')) {
    Copy-Item -LiteralPath (Join-Path $mingwBin $dll) -Destination (Join-Path $stageOutput $dll) -Force
}

if (-not $PromoteRuntime) {
    Write-Host "Native Apple Music helper built in isolation: $(Join-Path $stageOutput 'engine.exe')"
    Write-Host 'Runtime files were not replaced. Pass -PromoteRuntime after proof to promote this build.'
    return
}

Initialize-GlasswavePrivateRuntime
foreach ($name in @('engine.exe', 'libdrm_client.dll', 'libhybris-core.dll', 'hybris-linker\q.so')) {
    Copy-Item -LiteralPath (Join-Path $stageOutput $name) `
        -Destination (Join-Path $paths.RuntimeRoot $name) -Force
}
$runtimeStubs = Join-Path $paths.RuntimeRoot 'android-stubs'
New-Item -ItemType Directory -Force -Path $runtimeStubs | Out-Null
Copy-Item -Path (Join-Path $stubOutput '*') -Destination $runtimeStubs -Recurse -Force

$runtimeLib64 = Join-Path $paths.RuntimeRoot 'rootfs\system\lib64'
New-Item -ItemType Directory -Force -Path $runtimeLib64 | Out-Null
foreach ($stub in @('libandroid.so', 'libdl.so', 'libOpenSLES.so')) {
    Copy-Item -LiteralPath (Join-Path $stubOutput $stub) -Destination (Join-Path $runtimeLib64 $stub) -Force
}
foreach ($dll in @('libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll')) {
    Copy-Item -LiteralPath (Join-Path $mingwBin $dll) -Destination (Join-Path $paths.RuntimeRoot $dll) -Force
}

$configPath = Join-Path $paths.RuntimeRoot 'config.yaml'
if (-not (Test-Path -LiteralPath $configPath -PathType Leaf)) {
    $config = @(
        ('drm-binary-path: "{0}"' -f ($paths.RuntimeRoot -replace '\\', '/')),
        ('drm-base-dir: "{0}"' -f ((Join-Path $paths.RuntimeRoot 'files') -replace '\\', '/')),
        'alac-max: 192000',
        'get-m3u8-mode: hires',
        'get-m3u8-from-device: true',
        'aac-type: aac-lc',
        'exit-on-error: true'
    ) -join "`n"
    [IO.File]::WriteAllText($configPath, "$config`n", [Text.UTF8Encoding]::new($false))
    Set-GlasswavePrivateAcl $configPath
}
Write-GlasswaveRuntimeManifest
Write-Host "Native Apple Music helper promoted to runtime: $(Join-Path $paths.RuntimeRoot 'engine.exe')"
