param(
  [switch]$InstallBrokerDeps
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$brokerDir = Join-Path $repoRoot "broker"
$secretsPath = Join-Path $repoRoot "firmware\phase1_esp32\secrets.h"
$secretsExamplePath = Join-Path $repoRoot "firmware\phase1_esp32\secrets.example.h"

function Test-CommandExists {
  param([string]$Name)
  return [bool](Get-Command $Name -ErrorAction SilentlyContinue)
}

function Test-PathSafe {
  param([string]$Path)
  try {
    return Test-Path $Path
  } catch {
    return $false
  }
}

function Resolve-NodeExe {
  if (Test-CommandExists "node") {
    return (Get-Command node).Source
  }

  $candidates = @(
    "C:\Program Files\nodejs\node.exe",
    "C:\Users\$env:USERNAME\AppData\Local\Programs\nodejs\node.exe"
  )

  $wingetRoot = "C:\Users\$env:USERNAME\AppData\Local\Microsoft\WinGet\Packages"
  if (Test-PathSafe $wingetRoot) {
    $wingetCandidates = Get-ChildItem $wingetRoot -Directory -ErrorAction SilentlyContinue |
      Where-Object { $_.Name -like "OpenJS.NodeJS.LTS_*" } |
      ForEach-Object {
        Get-ChildItem $_.FullName -Directory -Filter "node-v*-win-x64" -ErrorAction SilentlyContinue |
          ForEach-Object { Join-Path $_.FullName "node.exe" }
      }
    $candidates += $wingetCandidates
  }

  foreach ($path in $candidates) {
    if (Test-PathSafe $path) {
      return $path
    }
  }

  return $null
}

function Resolve-NpmCmd {
  if (Test-CommandExists "npm") {
    return (Get-Command npm).Source
  }

  $nodeExe = Resolve-NodeExe
  if ($nodeExe) {
    $npmCmd = Join-Path (Split-Path -Parent $nodeExe) "npm.cmd"
    if (Test-PathSafe $npmCmd) {
      return $npmCmd
    }
  }

  return $null
}

function Resolve-ArduinoCliExe {
  if (Test-CommandExists "arduino-cli") {
    return (Get-Command arduino-cli).Source
  }

  $candidates = @(
    "C:\Program Files\Arduino CLI\arduino-cli.exe",
    "C:\Users\$env:USERNAME\AppData\Local\Programs\Arduino CLI\arduino-cli.exe"
  )

  foreach ($path in $candidates) {
    if (Test-PathSafe $path) {
      return $path
    }
  }

  return $null
}

Write-Host "== RC Car bootstrap check =="
Write-Host "Repo root: $repoRoot"

$nodeExe = Resolve-NodeExe
$npmCmd = Resolve-NpmCmd
$arduinoCliExe = Resolve-ArduinoCliExe
$hasNode = [bool]$nodeExe
$hasNpm = [bool]$npmCmd
$hasArduinoCli = [bool]$arduinoCliExe

Write-Host ""
Write-Host "Tools:"
Write-Host ("- node: " + ($(if ($hasNode) { "OK" } else { "MISSING" })))
Write-Host ("- npm: " + ($(if ($hasNpm) { "OK" } else { "MISSING" })))
Write-Host ("- arduino-cli: " + ($(if ($hasArduinoCli) { "OK" } else { "MISSING" })))
if ($hasNode) { Write-Host ("  node path: " + $nodeExe) }
if ($hasNpm) { Write-Host ("  npm path: " + $npmCmd) }
if ($hasArduinoCli) { Write-Host ("  arduino-cli path: " + $arduinoCliExe) }

if (-not (Test-PathSafe $secretsPath)) {
  Copy-Item $secretsExamplePath $secretsPath
  Write-Host ""
  Write-Host "Created firmware/phase1_esp32/secrets.h from template."
} else {
  Write-Host ""
  Write-Host "secrets.h already exists."
}

if ($InstallBrokerDeps) {
  if (-not $hasNpm) {
    Write-Host ""
    Write-Host "Skipping broker dependency install: npm is missing."
  } else {
    Write-Host ""
    Write-Host "Installing broker dependencies..."
    Push-Location $brokerDir
    try {
      & $npmCmd ci
    } finally {
      Pop-Location
    }
  }
}

Write-Host ""
Write-Host "Next steps:"
Write-Host "1) Fill firmware/phase1_esp32/secrets.h"
Write-Host "2) If needed, run: .\scripts\bootstrap.ps1 -InstallBrokerDeps"
Write-Host "3) Start broker: cd broker; npm start"
