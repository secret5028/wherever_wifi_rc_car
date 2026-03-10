$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$inputPath = Join-Path $repoRoot "web\index.html"
$outputPath = Join-Path $repoRoot "firmware\phase1_esp32\web_index.h"

$lines = Get-Content -Path $inputPath

$builder = New-Object System.Collections.Generic.List[string]
$builder.Add('#pragma once')
$builder.Add('// Auto-generated from web/index.html - do not edit manually')
$builder.Add('// Run tools/generate_web_index.ps1 after changing web/index.html')
$builder.Add('')
$builder.Add('#include <pgmspace.h>')
$builder.Add('')
$builder.Add('const char WEB_INDEX_HTML[] PROGMEM =')

foreach ($line in $lines) {
  $escaped = $line.Replace('\', '\\').Replace('"', '\"')
  $builder.Add('  "' + $escaped + ([string][char]92) + 'n"')
}

$builder.Add(';')

[System.IO.File]::WriteAllLines($outputPath, $builder)
