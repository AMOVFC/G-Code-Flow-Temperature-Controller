<#
.SYNOPSIS
    Build a matched A/B pair from one raw G-code file: old tool vs new tool.

.DESCRIPTION
    Processes the SAME input with the legacy SB53-Systems.exe and with flowtemp.exe,
    using the same calibration, then compares the temperature curves. The result is two
    files you can print back to back.

    Everything happens on COPIES. The legacy tool overwrites the file it is given and,
    if you close its window, deletes it -- so the original is never handed to it directly.

.PARAMETER Input
    The raw, unprocessed G-code to test with.

.PARAMETER Legacy
    Folder containing SB53-Systems.exe, its Config\ folder and its config.json.

.PARAMETER OutDir
    Where the pair is written. Defaults to .\testpair next to the input.

.PARAMETER SkipLegacy
    Only produce the new tool's output. Use this if driving the old GUI proves awkward
    and you would rather run it by hand.

.EXAMPLE
    ./tools/make-testpair.ps1 -Input C:\prints\benchy.gcode `
        -Legacy "D:\SB53_G-Code_Flow_Temperature_Controller_V1.1" `
        -Low 1 -Mid 80 -High 105 -LowTemp 220 -MidTemp 280 -HighTemp 310 -Bias 7
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$InputFile,
    [string]$Legacy,
    [string]$OutDir,
    [switch]$SkipLegacy,

    # Calibration. Read yours with: python tools/dump-profiles.py <Config.sdb>
    [double]$Low = 1, [double]$Mid = 80, [double]$High = 105,
    [double]$LowTemp = 220, [double]$MidTemp = 280, [double]$HighTemp = 310,
    [double]$Rise = 5, [double]$Fall = 1,
    [int]$Smoothing = 20,

    # NOTE: the legacy scale is inverted. If Config.sdb stores SPEED_QUALITY_OPT = 3,
    # pass 7 here. See ADR-0006.
    [int]$Bias = 7,

    [double]$CoolBelow = 5, [double]$CoolDrop = 0,
    [string]$Estimator
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

if (-not (Test-Path $InputFile)) { throw "Input not found: $InputFile" }
$InputFile = (Resolve-Path $InputFile).Path

$flowtemp = Join-Path $repo 'build\bin\flowtemp.exe'
if (-not (Test-Path $flowtemp)) { throw "flowtemp.exe not built. Run ./tools/build.ps1 first." }
if (-not $Estimator) { $Estimator = Join-Path $repo 'bin\klipper_estimator.exe' }

if (-not $OutDir) { $OutDir = Join-Path (Split-Path -Parent $InputFile) 'testpair' }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$base = [IO.Path]::GetFileNameWithoutExtension($InputFile)
$rawCopy = Join-Path $OutDir "$base-RAW.gcode"
$newOut  = Join-Path $OutDir "$base-NEW-flowtemp.gcode"
$oldOut  = Join-Path $OutDir "$base-OLD-legacy.gcode"

Copy-Item $InputFile $rawCopy -Force
Write-Host "input : $InputFile"
Write-Host "output: $OutDir`n"

# ---------------------------------------------------------------------------
# New tool
# ---------------------------------------------------------------------------
Write-Host "[1/3] flowtemp.exe ..." -ForegroundColor Cyan
& $flowtemp process $rawCopy --out $newOut --estimator $Estimator `
    --low $Low --mid $Mid --high $High `
    --low-temp $LowTemp --mid-temp $MidTemp --high-temp $HighTemp `
    --rise $Rise --fall $Fall --smoothing $Smoothing --bias $Bias `
    --cool-below $CoolBelow --cool-drop $CoolDrop
if ($LASTEXITCODE -ne 0) { throw "flowtemp failed" }

# ---------------------------------------------------------------------------
# Legacy tool
# ---------------------------------------------------------------------------
if ($SkipLegacy -or -not $Legacy) {
    Write-Host "`n[2/3] legacy skipped." -ForegroundColor DarkGray
    Write-Host "      To make the other half by hand: open SB53-Systems.exe, load"
    Write-Host "      $rawCopy, press PROCEED, then save as"
    Write-Host "      $oldOut"
    Write-Host "`nDone. New-tool output: $newOut"
    return
}

$legacyExe = Join-Path $Legacy 'SB53-Systems.exe'
if (-not (Test-Path $legacyExe)) { throw "SB53-Systems.exe not found in $Legacy" }

# The legacy refuses paths containing spaces, and it OVERWRITES whatever path it is
# given. Stage a copy at a short, space-free location.
$stage = 'C:\sb53pair'
New-Item -ItemType Directory -Force -Path $stage | Out-Null
$stageFile = Join-Path $stage 'in.gcode'
Copy-Item $rawCopy $stageFile -Force

Write-Host "`n[2/3] SB53-Systems.exe (legacy) ..." -ForegroundColor Cyan
Write-Host "      A window will appear and process automatically." -ForegroundColor DarkGray

$proc = Start-Process -FilePath $legacyExe -ArgumentList $stageFile -PassThru -WorkingDirectory $Legacy

# It shows a confirmation dialog before starting. Dismiss it.
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName Microsoft.VisualBasic
Start-Sleep -Seconds 2
try {
    [Microsoft.VisualBasic.Interaction]::AppActivate($proc.Id)
    Start-Sleep -Milliseconds 300
    [System.Windows.Forms.SendKeys]::SendWait('{ENTER}')
} catch {
    Write-Host "      (could not auto-dismiss the dialog - click through it yourself)" -ForegroundColor Yellow
}

# Wait for it to finish: the input file is overwritten in place once done, so watch for
# the processed marker rather than guessing a duration.
Write-Host "      waiting for it to finish (up to 5 min)..." -ForegroundColor DarkGray
$deadline = (Get-Date).AddMinutes(5)
$done = $false
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 3
    if (-not (Test-Path $stageFile)) { break }
    $head = Get-Content $stageFile -TotalCount 400 -ErrorAction SilentlyContinue
    if ($head -match '^; Edited by') {
        $sizeA = (Get-Item $stageFile).Length
        Start-Sleep -Seconds 2
        if ((Get-Item $stageFile).Length -eq $sizeA) { $done = $true; break }
    }
}

# Do NOT click Close in the legacy UI: BitBtn3Click deletes the input file when it was
# launched with a path argument. Killing the process avoids that entirely.
Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 400

if (-not $done -or -not (Test-Path $stageFile)) {
    Write-Host "      legacy run did not complete - producing the new file only." -ForegroundColor Yellow
} else {
    Copy-Item $stageFile $oldOut -Force
    $m104 = (Select-String -Path $oldOut -Pattern '^M104 S' -AllMatches).Count
    if ($m104 -eq 0) {
        Write-Host "      WARNING: the legacy output has NO M104 commands." -ForegroundColor Yellow
        Write-Host "      That is its silent-failure mode - the file looks processed but is not." -ForegroundColor Yellow
        Write-Host "      Do not print it. Check its Config profiles and config.json." -ForegroundColor Yellow
    } else {
        Write-Host "      ok - $m104 temperature commands" -ForegroundColor Green
    }
}
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue

# ---------------------------------------------------------------------------
# Compare
# ---------------------------------------------------------------------------
if (Test-Path $oldOut) {
    Write-Host "`n[3/3] comparing temperature curves ..." -ForegroundColor Cyan
    & $flowtemp compare $oldOut $newOut
    Write-Host "`nFeedrate comparison:" -ForegroundColor Cyan
    $py = Get-Command python -ErrorAction SilentlyContinue
    if ($py) { & python (Join-Path $repo 'tools\compare-feedrates.py') $rawCopy $oldOut $newOut }
}

Write-Host ""
if (Test-Path $oldOut) {
    Write-Host "Ready to print:" -ForegroundColor Green
    Write-Host "  OLD (legacy) : $oldOut"
    Write-Host "  NEW (C++)    : $newOut"
    Write-Host "  raw input    : $rawCopy"
} else {
    Write-Host "Only the NEW file was produced:" -ForegroundColor Yellow
    Write-Host "  NEW (C++)    : $newOut"
    Write-Host "  raw input    : $rawCopy"
    Write-Host ""
    Write-Host "The legacy half needs to be made by hand. Driving its window"
    Write-Host "automatically is unreliable - it opens a modal dialog and needs a real"
    Write-Host "interactive desktop, so it fails from scripts and scheduled tasks."
    Write-Host ""
    Write-Host "  1. Open SB53-Systems.exe" -ForegroundColor Cyan
    Write-Host "  2. Load $rawCopy" -ForegroundColor Cyan
    Write-Host "  3. Press PROCEED, wait for the chart to finish" -ForegroundColor Cyan
    Write-Host "  4. Save as $oldOut" -ForegroundColor Cyan
    Write-Host ""
    Write-Host "  Then compare:" -ForegroundColor Cyan
    Write-Host "    $flowtemp compare `"$oldOut`" `"$newOut`"" -ForegroundColor Cyan
    Write-Host ""
    Write-Host "  Check the saved file contains M104 commands. If it has the" -ForegroundColor Yellow
    Write-Host "  '; Edited by' header but no M104, the old tool failed silently" -ForegroundColor Yellow
    Write-Host "  and the file is NOT processed - do not print it." -ForegroundColor Yellow
}
