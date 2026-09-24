# build.ps1 -- compile WASTELAND firmware for Unihiker K10
# Usage: .\scripts\build.ps1
#
# Reproduces the verified flags from docs/build-run-test.txt: FQBN
# UNIHIKER:esp32:k10, build.cdc_on_boot=1. Does NOT upload -- pair with flash.ps1.
# Libraries come only from the repo-local .arduino\ sketchbook (pinned in
# _arduino-env.ps1, installed by setup_libs.ps1 on first run / version bump).

[CmdletBinding()]
param(
  [string]$Cli   = 'C:\Program Files\Arduino CLI\arduino-cli.exe',
  [string]$Sketch
)

$ErrorActionPreference = 'Stop'

# $PSScriptRoot is empty inside param() defaults under `powershell -File`
# (Windows PowerShell 5.1) -- resolve the sketch root in the body instead.
$here = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
. (Join-Path $here '_arduino-env.ps1')
if (-not $Sketch) { $Sketch = $RepoRoot }

if (-not (Test-Path $Cli)) {
  Write-Error "arduino-cli not found at $Cli. Install Arduino CLI or pass -Cli <path>."
}

if (-not (Test-PinnedLibsCurrent)) {
  Write-Host "[build] pinned libraries missing or out of date -- running setup_libs.ps1"
  & (Join-Path $here 'setup_libs.ps1') -Cli $Cli
}
Use-PinnedLibs
$sketchDir = Resolve-SketchDir $Sketch

Write-Host "[build] sketch : $sketchDir"
Write-Host "[build] libs   : $LibRoot"
Write-Host "[build] cli    : $Cli"
Write-Host "[build] fqbn   : $Fqbn  (cdc_on_boot=1)"

# build_opt.h reaches every compile (sketch, libraries, core) as "@file", which
# arduino-cli's dependency tracking can't see -- edit it and the cached library
# objects keep the old flags. Force a clean build whenever its hash changes.
$compileArgs = @('compile', '--fqbn', $Fqbn, '--build-property', 'build.cdc_on_boot=1') + (Get-SdkBuildProperty $Cli)
$optFile  = Join-Path $Sketch 'build_opt.h'
$optStamp = Join-Path $LibRoot 'build_opt.sha256'
$optHash  = if (Test-Path $optFile) { (Get-FileHash $optFile -Algorithm SHA256).Hash } else { 'none' }
$lastHash = if (Test-Path $optStamp) { (Get-Content $optStamp -Raw).Trim() } else { '' }
if ($optHash -ne $lastHash) {
  Write-Host "[build] build_opt.h changed -- clean build"
  $compileArgs += '--clean'
}

& $Cli @compileArgs $sketchDir

if ($LASTEXITCODE -ne 0) { Write-Error "compile failed (exit $LASTEXITCODE)" }
Set-Content -Path $optStamp -Value $optHash -Encoding ascii
Write-Host "[build] OK"
