<#
.SYNOPSIS
    Builds Assimp from source and installs the result into the plugin's vendored ThirdParty tree.

.DESCRIPTION
    AssimpForUnreal ships prebuilt Assimp binaries so the plugin compiles on clone with no setup
    step. This script regenerates those binaries. You only need to run it when bumping the Assimp
    version, changing the build configuration, or adding a platform.

    Outputs (committed to the repo via Git LFS):
        Source/ThirdParty/AssimpLibrary/include/assimp/**    public headers (+ generated config.h)
        Source/ThirdParty/AssimpLibrary/lib/<Platform>/assimp.lib
        Source/ThirdParty/AssimpLibrary/bin/<Platform>/assimp.dll
        Source/ThirdParty/AssimpLibrary/AssimpVersion.json   provenance manifest

    Output naming
    -------------
    By default Assimp emits a toolset-tagged name such as 'assimp-vc143-mt.dll' or
    'assimp-vc145-mt.dll', because code/CMakeLists.txt does:

        set(LIBRARY_SUFFIX "${ASSIMP_LIBRARY_SUFFIX}-${MSVC_PREFIX}-mt" CACHE STRING ...)
        SET_TARGET_PROPERTIES(assimp PROPERTIES OUTPUT_NAME assimp${LIBRARY_SUFFIX})

    That would force AssimpLibrary.Build.cs to guess which MSVC toolset produced the binaries.
    Because LIBRARY_SUFFIX is a CACHE entry declared without FORCE, passing -DLIBRARY_SUFFIX= on
    the command line pre-seeds the cache and the set() above leaves it alone, so the target builds
    as a plain 'assimp'. We normalise at CONFIGURE time rather than by renaming afterwards: an MSVC
    import library records the DLL filename to load at runtime, so renaming a built DLL yields a
    library that links successfully but fails to load at runtime.

.PARAMETER AssimpTag
    Git tag to build. Defaults to the version this plugin is developed against.

.PARAMETER Platform
    Target platform directory name. Only Win64 is supported today.

.PARAMETER Clean
    Delete the cached source clone and build directory before building.

.EXAMPLE
    ./Scripts/BuildAssimp.ps1

.EXAMPLE
    ./Scripts/BuildAssimp.ps1 -AssimpTag v6.0.5 -Clean
#>
[CmdletBinding()]
param(
    [string] $AssimpTag = 'v6.0.5',

    [ValidateSet('Win64')]
    [string] $Platform = 'Win64',

    [switch] $Clean
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# ---------------------------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------------------------
$ScriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Path
$PluginRoot = Split-Path -Parent $ScriptDir
$ThirdParty = Join-Path $PluginRoot 'Source/ThirdParty/AssimpLibrary'

$SrcDir   = Join-Path $ScriptDir '.assimp-src'
$BuildDir = Join-Path $ScriptDir '.assimp-build'

$OutInclude = Join-Path $ThirdParty 'include'
$OutLib     = Join-Path $ThirdParty "lib/$Platform"
$OutBin     = Join-Path $ThirdParty "bin/$Platform"

function Write-Step { param([string] $Message) Write-Host "`n==> $Message" -ForegroundColor Cyan }
function Write-Note { param([string] $Message) Write-Host "    $Message" -ForegroundColor DarkGray }

function Assert-Tool {
    param([string] $Name, [string] $Hint)
    $Command = Get-Command $Name -ErrorAction SilentlyContinue
    if (-not $Command) { throw "'$Name' was not found on PATH. $Hint" }
    return $Command.Source
}

# ---------------------------------------------------------------------------------------------
# Prerequisites
# ---------------------------------------------------------------------------------------------
Write-Step 'Checking prerequisites'
Write-Note ('git   : ' + (Assert-Tool 'git'   'Install Git for Windows.'))
Write-Note ('cmake : ' + (Assert-Tool 'cmake' 'Install CMake 3.22 or newer and add it to PATH.'))
Write-Note "plugin root : $PluginRoot"
Write-Note "platform    : $Platform"

if ($Clean) {
    Write-Step 'Cleaning cached source and build directories'
    foreach ($Dir in @($SrcDir, $BuildDir)) {
        if (Test-Path $Dir) {
            Write-Note "removing $Dir"
            Remove-Item -Recurse -Force $Dir
        }
    }
}

# ---------------------------------------------------------------------------------------------
# Fetch source at the pinned tag
# ---------------------------------------------------------------------------------------------
Write-Step "Fetching Assimp $AssimpTag"
if (-not (Test-Path $SrcDir)) {
    # A shallow single-tag clone is all we need, and keeps this fast.
    & git clone --depth 1 --branch $AssimpTag 'https://github.com/assimp/assimp.git' $SrcDir
    if ($LASTEXITCODE -ne 0) { throw "git clone failed for tag '$AssimpTag'." }
}
else {
    Write-Note 'source directory already present; fetching tag'
    $RefSpec = 'refs/tags/{0}:refs/tags/{0}' -f $AssimpTag
    & git -C $SrcDir fetch --depth 1 origin $RefSpec
    if ($LASTEXITCODE -ne 0) { throw "git fetch failed for tag '$AssimpTag'." }
    & git -C $SrcDir checkout --force $AssimpTag
    if ($LASTEXITCODE -ne 0) { throw "git checkout failed for tag '$AssimpTag'." }
}

$AssimpCommit = (& git -C $SrcDir rev-parse HEAD).Trim()
Write-Note "commit : $AssimpCommit"

# ---------------------------------------------------------------------------------------------
# Configure
# ---------------------------------------------------------------------------------------------
# Rationale for each flag:
#   LIBRARY_SUFFIX=              Build as plain 'assimp.dll'/'assimp.lib' (see .DESCRIPTION).
#   BUILD_SHARED_LIBS=ON         Ship a DLL, not a static lib. Assimp bundles its own zlib;
#                                linking that statically into Unreal collides with the engine's
#                                zlib symbols. A DLL keeps Assimp's zlib private to the DLL.
#   USE_STATIC_CRT=OFF           Unreal compiles against the dynamic CRT (/MD). These must match.
#   ASSIMP_NO_EXPORT=OFF         Build the exporters in. They add roughly 20% to the DLL, which is the
#                                price of the export API; without them Assimp::Exporter reports zero
#                                formats and every export call fails at runtime rather than at build
#                                time, which is a much worse way to find out.
#   ASSIMP_BUILD_ZLIB=ON         Use Assimp's vendored zlib so the DLL is self-contained.
#   ASSIMP_IGNORE_GIT_HASH=ON    Keeps the build reproducible.
#   ASSIMP_INJECT_DEBUG_POSTFIX=OFF  One output filename across configurations.
Write-Step 'Configuring (CMake)'
$CMakeArgs = @(
    '-S', $SrcDir
    '-B', $BuildDir
    '-A', 'x64'
    '-DLIBRARY_SUFFIX='
    '-DCMAKE_BUILD_TYPE=Release'
    '-DBUILD_SHARED_LIBS=ON'
    '-DUSE_STATIC_CRT=OFF'
    '-DASSIMP_NO_EXPORT=OFF'
    '-DASSIMP_BUILD_ZLIB=ON'
    # Draco-compressed glTF is common in the wild (Sketchfab and friends export it by default) and
    # Assimp refuses those files outright without it. The source is bundled in contrib/draco, so
    # nothing is fetched at build time.
    #
    # _STATIC, not plain ASSIMP_BUILD_DRACO. With BUILD_SHARED_LIBS=ON set globally, Draco honours it
    # and builds as its own draco.dll -- assimp.dll then carries an unsatisfied import and fails to
    # load at all ("Missing import: draco.dll"), taking every format down with it, not just Draco.
    # The _STATIC variant links Draco into assimp.dll and implies ASSIMP_BUILD_DRACO.
    '-DASSIMP_BUILD_DRACO_STATIC=ON'
    '-DASSIMP_BUILD_TESTS=OFF'
    '-DASSIMP_BUILD_SAMPLES=OFF'
    '-DASSIMP_BUILD_ASSIMP_TOOLS=OFF'
    '-DASSIMP_BUILD_ASSIMP_VIEW=OFF'
    '-DASSIMP_INSTALL=OFF'
    '-DASSIMP_WARNINGS_AS_ERRORS=OFF'
    '-DASSIMP_IGNORE_GIT_HASH=ON'
    '-DASSIMP_INJECT_DEBUG_POSTFIX=OFF'
    '-DASSIMP_BUILD_DOCS=OFF'
    '-DASSIMP_OPT_BUILD_PACKAGES=OFF'
)
Write-Note ($CMakeArgs -join ' ')
& cmake @CMakeArgs
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }

# ---------------------------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------------------------
Write-Step 'Building (Release)'
& cmake --build $BuildDir --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw 'CMake build failed.' }

# ---------------------------------------------------------------------------------------------
# Locate outputs
# ---------------------------------------------------------------------------------------------
Write-Step 'Locating build outputs'

function Find-BuildOutput {
    param([string] $Pattern, [string] $Kind)

    $Found = @(
        Get-ChildItem -Path $BuildDir -Recurse -Filter $Pattern -File -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -notmatch '[\\/]contrib[\\/]' } |
            Sort-Object LastWriteTime -Descending
    )

    if ($Found.Count -eq 0) {
        throw "Could not find the built $Kind matching '$Pattern' under $BuildDir."
    }
    if ($Found.Count -gt 1) {
        Write-Note "multiple $Kind candidates; taking the newest:"
        $Found | ForEach-Object { Write-Note "  $($_.FullName)" }
    }
    return $Found[0]
}

# -DLIBRARY_SUFFIX= should have produced exactly 'assimp.dll' / 'assimp.lib'. Fail loudly if not,
# rather than silently vendoring a toolset-tagged binary that Build.cs will not find.
$BuiltDll = Find-BuildOutput -Pattern 'assimp.dll' -Kind 'shared library'
$BuiltLib = Find-BuildOutput -Pattern 'assimp.lib' -Kind 'import library'

# Collect dependency DLLs that must ship alongside assimp.dll.
#
# Assimp's contrib dependencies honour the global BUILD_SHARED_LIBS=ON, so Draco builds as its own
# draco.dll even with ASSIMP_BUILD_DRACO_STATIC=ON (that option only changes compile flags, not the
# library type). assimp.dll then carries an import for it, and if the DLL is not shipped assimp.dll
# fails to load at all -- silently disabling every format, not just the Draco ones.
#
# So vendor them rather than fight CMake: they are recorded in the manifest, staged by
# AssimpLibrary.Build.cs, and loaded by AssimpCore BEFORE assimp.dll so the import resolves from an
# already-loaded module rather than depending on the OS search path.
$DependencyDlls = @(
    Get-ChildItem -Path $BuildDir -Recurse -Filter '*.dll' -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -ne 'assimp.dll' } |
        Sort-Object Name -Unique
)
if ($DependencyDlls.Count -gt 0) {
    Write-Note ("dependency DLLs: " + (($DependencyDlls | ForEach-Object { $_.Name }) -join ', '))
}
Write-Note "dll : $($BuiltDll.FullName)"
Write-Note "lib : $($BuiltLib.FullName)"

# ---------------------------------------------------------------------------------------------
# Install headers
# ---------------------------------------------------------------------------------------------
Write-Step 'Installing headers'
if (Test-Path $OutInclude) { Remove-Item -Recurse -Force $OutInclude }
New-Item -ItemType Directory -Force -Path $OutInclude | Out-Null

# Upstream public headers.
Copy-Item -Recurse -Force (Join-Path $SrcDir 'include/assimp') (Join-Path $OutInclude 'assimp')

# CMake generates config.h (and revision.h) into the build tree, not the source tree. Without
# them the vendored headers do not compile.
$Generated = @(
    Get-ChildItem -Path $BuildDir -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -in @('config.h', 'revision.h') -and $_.FullName -match 'assimp' }
)
foreach ($Header in $Generated) {
    $Destination = Join-Path $OutInclude "assimp/$($Header.Name)"
    Write-Note "generated header : $($Header.Name)"
    Copy-Item -Force $Header.FullName $Destination
}
if (-not (Test-Path (Join-Path $OutInclude 'assimp/config.h'))) {
    throw 'assimp/config.h was not produced by the build; the vendored headers would not compile.'
}

# ---------------------------------------------------------------------------------------------
# Install binaries
# ---------------------------------------------------------------------------------------------
Write-Step "Installing binaries for $Platform"
foreach ($Dir in @($OutLib, $OutBin)) {
    if (-not (Test-Path $Dir)) { New-Item -ItemType Directory -Force -Path $Dir | Out-Null }
}
# Clear the bin directory first: a stale dependency DLL from a previous configuration would be
# staged and loaded even though nothing imports it any more.
Get-ChildItem -Path $OutBin -Filter '*.dll' -File -ErrorAction SilentlyContinue | Remove-Item -Force

Copy-Item -Force $BuiltLib.FullName (Join-Path $OutLib  'assimp.lib')
Copy-Item -Force $BuiltDll.FullName (Join-Path $OutBin  'assimp.dll')
Write-Note (Join-Path $OutLib 'assimp.lib')
Write-Note (Join-Path $OutBin 'assimp.dll')

foreach ($Dependency in $DependencyDlls) {
    Copy-Item -Force $Dependency.FullName (Join-Path $OutBin $Dependency.Name)
    Write-Note (Join-Path $OutBin $Dependency.Name)
}

# ---------------------------------------------------------------------------------------------
# Provenance manifest
# ---------------------------------------------------------------------------------------------
Write-Step 'Writing provenance manifest'
$Manifest = [ordered]@{
    AssimpTag     = $AssimpTag
    AssimpCommit  = $AssimpCommit
    Platform      = $Platform
    LibName       = 'assimp.lib'
    DllName       = 'assimp.dll'
    DependencyDlls = @($DependencyDlls | ForEach-Object { $_.Name })
    Configuration = 'Release'
    CMakeArgs     = $CMakeArgs
    CMakeVersion  = ((& cmake --version) | Select-Object -First 1)
    BuiltBy       = "$env:USERNAME on $env:COMPUTERNAME"
}
$ManifestPath = Join-Path $ThirdParty 'AssimpVersion.json'
$Manifest | ConvertTo-Json -Depth 4 | Set-Content -Path $ManifestPath -Encoding utf8
Write-Note $ManifestPath

Write-Host "`nDone. Assimp $AssimpTag installed into the vendored ThirdParty tree." -ForegroundColor Green
Write-Host "Remember to 'git add' the LFS-tracked binaries and AssimpVersion.json." -ForegroundColor Yellow
