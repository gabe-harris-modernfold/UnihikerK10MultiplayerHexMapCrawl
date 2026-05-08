# build.ps1 -- compile WASTELAND firmware for Unihiker K10
# Usage: .\scripts\build.ps1
#
# Reproduces the verified flags from docs/build-run-test.txt: FQBN
# UNIHIKER:esp32:k10, build.cdc_on_boot=1. Does NOT upload -- pair with flash.ps1.

[CmdletBinding()]
param(
  [string]$Cli   = 'C:\Program Files\Arduino CLI\arduino-cli.exe',
  [string]$Sketch = (Resolve-Path "$PSScriptRoot\..").Path
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Cli)) {
  Write-Error "arduino-cli not found at $Cli. Install Arduino CLI or pass -Cli <path>."
}

Write-Host "[build] sketch : $Sketch"
Write-Host "[build] cli    : $Cli"
Write-Host "[build] fqbn   : UNIHIKER:esp32:k10  (cdc_on_boot=1)"

& $Cli compile `
  --fqbn UNIHIKER:esp32:k10 `
  --build-property "build.cdc_on_boot=1" `
  $Sketch

if ($LASTEXITCODE -ne 0) { Write-Error "compile failed (exit $LASTEXITCODE)" }
Write-Host "[build] OK"
