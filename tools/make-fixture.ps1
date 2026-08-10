<#
.SYNOPSIS
    Build a small, structurally complete G-code fixture from a large real file.

.DESCRIPTION
    Real sliced files are 1.5-2.5 MB each, which is too large to commit and too slow to
    iterate on. But the scanner cares about *structure*, not volume: the header, the
    first body marker, a representative run of moves, and the end-of-print marker.

    This keeps the header verbatim, a slice of the print body, and the footer verbatim,
    producing a file of a few hundred lines that still exercises every boundary the
    scanner has to find.

    The result is NOT printable and must never be sent to a printer. It exists to be
    parsed.

.EXAMPLE
    ./tools/make-fixture.ps1 -Input C:\...\benchyrawgcode.gcode -Output testdata\fixtures\orca-basic.gcode
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$InputFile,
    [Parameter(Mandatory)][string]$Output,
    [int]$BodyLines = 400
)

$ErrorActionPreference = 'Stop'

$lines = [System.IO.File]::ReadAllLines($InputFile)

# The print body starts at the first layer-height marker and ends at the executable
# block end. See ALGORITHM.md ?9.
$bodyStart = -1
$bodyEnd = -1
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($bodyStart -lt 0 -and ($lines[$i] -match '^;HEIGHT:' -or $lines[$i] -match '^; Z_HEIGHT:')) {
        $bodyStart = $i
    }
    if ($lines[$i] -match 'EXECUTABLE_BLOCK_END' -or $lines[$i] -match '^; PRINT_END') {
        $bodyEnd = $i
        break
    }
}

if ($bodyStart -lt 0) { throw "No body-start marker (;HEIGHT: or ; Z_HEIGHT:) found in $InputFile" }
if ($bodyEnd -lt 0)   { throw "No body-end marker (EXECUTABLE_BLOCK_END or ; PRINT_END) found in $InputFile" }

$take = [Math]::Min($BodyLines, $bodyEnd - $bodyStart)

$out = New-Object System.Collections.Generic.List[string]
$out.Add("; TRIMMED FIXTURE - NOT PRINTABLE")
$out.Add("; Derived from: $(Split-Path -Leaf $InputFile)")
$out.Add("; Header and footer are verbatim; the print body is truncated to $take lines.")
$out.Add("; Regenerate with tools/make-fixture.ps1")
$out.Add(";")

# Header, verbatim.
$out.AddRange([string[]]$lines[0..($bodyStart - 1)])
# A slice of the body.
$out.AddRange([string[]]$lines[$bodyStart..($bodyStart + $take - 1)])
# Footer, verbatim.
$out.AddRange([string[]]$lines[$bodyEnd..($lines.Count - 1)])

$dir = Split-Path -Parent $Output
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }

# LF line endings, no BOM: fixtures are compared literally.
$text = ($out -join "`n") + "`n"
[System.IO.File]::WriteAllText($Output, $text, (New-Object System.Text.UTF8Encoding $false))

Write-Host ("{0} -> {1} ({2} lines, {3:N1} KB)" -f `
    (Split-Path -Leaf $InputFile), $Output, $out.Count, ((Get-Item $Output).Length / 1KB))
