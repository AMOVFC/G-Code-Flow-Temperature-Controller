<#
.SYNOPSIS
    Fail the build if a copyleft-licensed dependency has crept in.

.DESCRIPTION
    This project is MIT. Two real traps have already been avoided and both would have
    relicensed the whole thing silently, with nothing visible at code review:

      - Qt Charts and QCustomPlot are GPL-3.0-or-commercial, NOT part of Qt's LGPL
        modules. Linking either would make this project GPL-3.0 (ADR-0004).
      - Klipper itself is GPL-3.0. The in-process motion planner must be written from
        the documented algorithm, never transliterated from its source (ADR-0007).

    A human reviewer will not catch a `find_package(Qt6 COMPONENTS Charts)` buried in a
    CMake file six months from now. This will.
#>
[CmdletBinding()]
param([string]$Root)

$ErrorActionPreference = 'Stop'

# $PSScriptRoot is not reliably populated inside a param default block, so resolve here.
if (-not $Root) {
    $Root = if ($PSScriptRoot) { Split-Path -Parent $PSScriptRoot } else { (Get-Location).Path }
}
$failures = @()

# --- 1. Forbidden dependencies by name --------------------------------------
$forbidden = @(
    @{ Pattern = 'Qt6?::Charts|COMPONENTS\s+[^)]*Charts|QtCharts|QChartView'
       Why     = 'Qt Charts is GPL-3.0-or-commercial; it would relicense this MIT project (ADR-0004)' }
    @{ Pattern = 'qcustomplot|QCustomPlot'
       Why     = 'QCustomPlot is GPL-3.0-or-commercial (ADR-0004)' }
    @{ Pattern = 'FetchContent_Declare\s*\(\s*klipper'
       Why     = 'Klipper is GPL-3.0; the planner must be written from the documented algorithm, not vendored (ADR-0007)' }
)

$sources = Get-ChildItem -Path $Root -Recurse -File -Include *.cpp,*.hpp,*.h,*.txt,*.cmake `
    -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -notmatch '\\(build|_deps|\.git|Source)\\' }

foreach ($rule in $forbidden) {
    $hits = $sources | Select-String -Pattern $rule.Pattern -ErrorAction SilentlyContinue
    foreach ($hit in $hits) {
        $failures += "$($hit.Path):$($hit.LineNumber) -- $($rule.Why)"
    }
}

# --- 2. Copyleft notices in anything we vendor ------------------------------
# A dropped-in third-party source file is the other way this happens.
$vendored = Get-ChildItem -Path $Root -Recurse -File -Include *.c,*.cc,*.cpp,*.h,*.hpp `
    -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match 'third_party|vendor|external' }

foreach ($file in $vendored) {
    $head = Get-Content $file.FullName -TotalCount 60 -ErrorAction SilentlyContinue
    if ($head -match 'GNU General Public License|GPL-3\.0|AGPL|GNU Affero') {
        $failures += "$($file.FullName) -- vendored file carries a GPL/AGPL notice"
    }
}

# --- 3. The project licence itself must not have changed --------------------
$licence = Join-Path $Root 'LICENSE'
if (Test-Path $licence) {
    if (-not (Select-String -Path $licence -Pattern 'MIT License' -Quiet)) {
        $failures += 'LICENSE is no longer the MIT License -- if that is intended, update this check and the ADRs'
    }
} else {
    $failures += 'LICENSE file is missing'
}

# --- report -----------------------------------------------------------------
if ($failures.Count -gt 0) {
    Write-Host ''
    Write-Host 'LICENCE COMPLIANCE FAILED' -ForegroundColor Red
    Write-Host ''
    $failures | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    Write-Host ''
    Write-Host 'This project is MIT. Adding a GPL dependency relicenses it for everyone'
    Write-Host 'who redistributes it, and the change is invisible at code review.'
    Write-Host 'See docs/adr/0004-custom-chart-widget.md and 0007-own-motion-planner.md.'
    Write-Host ''
    exit 1
}

Write-Host "licence compliance: OK ($($sources.Count) files checked, project is MIT)" -ForegroundColor Green
