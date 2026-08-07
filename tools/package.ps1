<#
.SYNOPSIS
    Assemble a self-contained, ready-to-run folder.

.DESCRIPTION
    Puts flowtemp.exe, the estimator and a printer config.json in ONE directory, so
    auto-detection works and there is no ambiguity about which config is in use.

    This exists because the repository has two `bin` directories -- `bin/` holds the
    legacy application, `build/bin/` holds the new one -- and because the config.json
    shipped in `bin/` describes the ORIGINAL AUTHOR's printer, not yours. Running with
    it silently produces wrong flow numbers.

.PARAMETER Config
    Path to your printer's config.json. If omitted, one is taken from
    testdata/printer-configs/ and the choice is reported.

.EXAMPLE
    ./tools/package.ps1 -Destination C:\flowtemp
#>
[CmdletBinding()]
param(
    [string]$Destination = "C:\flowtemp",
    [string]$Config
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

$exe = Join-Path $repo 'build\bin\flowtemp.exe'
if (-not (Test-Path $exe)) { throw "flowtemp.exe not built. Run ./tools/build.ps1 first." }

$estimator = Join-Path $repo 'bin\klipper_estimator.exe'
if (-not (Test-Path $estimator)) { throw "klipper_estimator.exe missing from $repo\bin" }

if (-not $Config) {
    $Config = Join-Path $repo 'testdata\printer-configs\awd-v0.json'
    Write-Host "No -Config given; using $Config" -ForegroundColor Yellow
}
if (-not (Test-Path $Config)) { throw "Config not found: $Config" }

New-Item -ItemType Directory -Force -Path $Destination | Out-Null
Copy-Item $exe        (Join-Path $Destination 'flowtemp.exe') -Force
Copy-Item $estimator  (Join-Path $Destination 'klipper_estimator.exe') -Force
Copy-Item $Config     (Join-Path $Destination 'config.json') -Force

Write-Host ""
Write-Host "Packaged to $Destination" -ForegroundColor Green
Get-ChildItem $Destination | ForEach-Object { "  {0,-24} {1,8:N0} KB" -f $_.Name, ($_.Length/1KB) }

Write-Host ""
Write-Host "Printer limits in the bundled config.json:" -ForegroundColor Cyan
Get-Content (Join-Path $Destination 'config.json') |
    Select-String 'max_velocity|max_acceleration|square_corner_velocity' |
    Select-Object -First 3 | ForEach-Object { "  " + $_.Line.Trim() }
Write-Host "  ^ confirm these match your printer.cfg before trusting the output." -ForegroundColor Yellow

Write-Host ""
Write-Host "Run it:" -ForegroundColor Green
Write-Host "  $Destination\flowtemp.exe serve"
Write-Host "  then open http://127.0.0.1:8765"
Write-Host ""
Write-Host "Everything is in one folder, so the estimator and config are found"
Write-Host "automatically -- no --estimator flag needed."
