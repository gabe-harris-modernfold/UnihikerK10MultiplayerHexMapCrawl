# setup_libs.ps1 -- install the pinned Arduino libraries into repo-local .arduino\
# Usage: .\scripts\setup_libs.ps1          (build.ps1 calls this when needed)
#        .\scripts\setup_libs.ps1 -Force   (wipe .arduino\libraries and reinstall)
#
# Versions live in _arduino-env.ps1 ($PinnedLibs). Also applies the one local
# patch LovyanGFX needs on this core -- see docs/dev-loop.md "Library pinning".

[CmdletBinding()]
param(
  [string]$Cli = 'C:\Program Files\Arduino CLI\arduino-cli.exe',
  [switch]$Force
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '_arduino-env.ps1')

if (-not (Test-Path $Cli)) {
  Write-Error "arduino-cli not found at $Cli. Install Arduino CLI or pass -Cli <path>."
}

Use-PinnedLibs
$libDir = Join-Path $LibRoot 'libraries'
if ($Force -and (Test-Path $libDir)) { Remove-Item -Recurse -Force $libDir }
New-Item -ItemType Directory -Force -Path $libDir | Out-Null

Write-Host "[libs] sketchbook: $LibRoot"
& $Cli lib update-index | Out-Null
foreach ($lib in $PinnedLibs) {
  Write-Host "[libs] $lib"
  # --no-deps: every dependency we want is pinned explicitly above.
  & $Cli lib install --no-deps $lib
  if ($LASTEXITCODE -ne 0) { Write-Error "lib install failed: $lib" }
}

# LovyanGFX: the ESP32-S3 RGB-panel sources #include <hal/gdma_ll.h>, which
# collides with the SDK's esp_private/gdma.h on the UNIHIKER 0.0.3 core. The
# K10 is an SPI ILI9341, so they're unused -- disable them.
$rgb = Join-Path $libDir 'LovyanGFX\src\lgfx\v1\platforms\esp32s3'
foreach ($f in 'Bus_RGB.cpp', 'Panel_RGB.cpp') {
  $p = Join-Path $rgb $f
  if (Test-Path $p) {
    Move-Item -Force $p "$p.disabled"
    Write-Host "[libs] disabled LovyanGFX $f"
  }
}

Set-Content -Path $LibStampFile -Value ($PinnedLibs -join "`n") -Encoding ascii
& $Cli lib list
Write-Host "[libs] OK"
