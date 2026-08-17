<#
.SYNOPSIS
    Configure, build and test the project.

.DESCRIPTION
    Locates Visual Studio's bundled CMake, Ninja and MSVC toolchain via vswhere, so no
    separate CMake install and no "Developer Prompt" is required.

    This script exists because none of cmake, ninja or cl are on PATH in a normal shell
    on a machine with only Visual Studio installed -- a detail that otherwise costs
    fifteen minutes to rediscover after a long gap.

.EXAMPLE
    ./tools/build.ps1
    ./tools/build.ps1 -Clean
    ./tools/build.ps1 -Config Debug -NoTests
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Config = 'RelWithDebInfo',

    [switch]$Clean,
    [switch]$NoTests
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repo 'build'

# --- locate Visual Studio ---------------------------------------------------
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found. Visual Studio 2019 or later is required."
}

$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) {
    throw "No Visual Studio installation with the C++ toolchain was found."
}

$cmake = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ctest = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe'
$ninja = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'

foreach ($tool in @($cmake, $ctest, $ninja, $vcvars)) {
    if (-not (Test-Path $tool)) {
        throw "Required tool not found: $tool`nInstall the 'C++ CMake tools for Windows' component."
    }
}

Write-Host "Visual Studio : $vsPath"
Write-Host "Configuration : $Config"
Write-Host ""

if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "Removing $buildDir"
    Remove-Item -Recurse -Force $buildDir
}

# --- run inside a developer environment -------------------------------------
# vcvars64.bat must be sourced by cmd, so the whole sequence runs there. Quoting is
# awkward but the alternative (parsing and importing the environment) is worse.
$steps = @(
    "`"$cmake`" -S `"$repo`" -B `"$buildDir`" -G Ninja -DCMAKE_MAKE_PROGRAM=`"$ninja`" -DCMAKE_BUILD_TYPE=$Config"
    "`"$cmake`" --build `"$buildDir`""
)
if (-not $NoTests) {
    $steps += "`"$cmake`" -E chdir `"$buildDir`" `"$ctest`" --output-on-failure"
}

$script = "call `"$vcvars`" >nul 2>&1 && " + ($steps -join ' && ')

# Tee to a log file. PowerShell's NativeCommandError handling can swallow a failing
# native command's output, which turns a one-line compiler error into a guessing game --
# the log is always there regardless.
$log = Join-Path $buildDir 'build.log'
if (-not (Test-Path $buildDir)) { New-Item -ItemType Directory -Force -Path $buildDir | Out-Null }

& cmd /c "$script" 2>&1 | Tee-Object -FilePath $log
$exit = $LASTEXITCODE

if ($exit -ne 0) {
    Write-Host ""
    Write-Host "BUILD FAILED (exit $exit). Errors:" -ForegroundColor Red
    $errors = Select-String -Path $log -Pattern 'error [A-Z]+[0-9]+|FAILED:|Errors while' |
              Select-Object -First 20
    if ($errors) {
        $errors | ForEach-Object { Write-Host "  $($_.Line.Trim())" -ForegroundColor Red }
    } else {
        Write-Host "  (no recognisable error lines; see $log)" -ForegroundColor Red
    }
    Write-Host ""
    throw "Build failed with exit code $exit. Full log: $log"
}

Write-Host ""
Write-Host "OK. Binaries in $buildDir\bin" -ForegroundColor Green
