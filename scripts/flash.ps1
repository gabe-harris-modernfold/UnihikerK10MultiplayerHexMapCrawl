# flash.ps1 -- upload compiled firmware to a Unihiker K10
# Usage: .\scripts\flash.ps1                # auto-detect COM port
#        .\scripts\flash.ps1 -Port COM3     # explicit
#
# If you haven't built yet, run .\scripts\build.ps1 first (or pass -Build to
# chain compile + upload in one shot).

[CmdletBinding()]
param(
  [string]$Cli   = 'C:\Program Files\Arduino CLI\arduino-cli.exe',
  [string]$Sketch = (Resolve-Path "$PSScriptRoot\..").Path,
  [string]$Port,
  [switch]$Build
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Cli)) {
  Write-Error "arduino-cli not found at $Cli. Install Arduino CLI or pass -Cli <path>."
}

if (-not $Port) {
  Write-Host "[flash] discovering board (USB VID scan)..."
  # Espressif native USB = 303A; CP210x = 10C4; CH340 = 1A86; FTDI = 0403.
  # arduino-cli board list also reports the Intel AMT Serial-over-LAN device
  # ("Serial Port Unknown" on COM3 on Dell business laptops) which is a
  # *false positive*, so we ignore arduino-cli and key off USB VID instead.
  $vids = '303A','10C4','1A86','0403'
  $cand = Get-PnpDevice -PresentOnly -Class Ports -ErrorAction SilentlyContinue |
          Where-Object {
            $instId = $_.InstanceId
            ($vids | ForEach-Object { $instId -match "VID_$_" }) -contains $true
          }
  if ($cand) {
    # FriendlyName is "USB Serial Device (COM6)" or similar.
    $m = [regex]::Match($cand[0].FriendlyName, '\((COM\d+)\)')
    if ($m.Success) { $Port = $m.Groups[1].Value }
  }
  if ($Port) {
    Write-Host "[flash] auto-detected port: $Port  ($($cand[0].FriendlyName))"
  } else {
    Write-Host "[flash] no Espressif/CH340/CP210x USB device found."
    Write-Host "[flash] If your K10 is plugged in, check the cable (must carry data, not just power)"
    Write-Host "[flash] and bypass any USB hub/dock. Pass -Port COMx explicitly to override."
    & $Cli board list
    Write-Error "Could not auto-detect a Unihiker K10."
  }
}

if ($Build) {
  & "$PSScriptRoot\build.ps1" -Cli $Cli -Sketch $Sketch
}

Write-Host "[flash] uploading to $Port ..."
& $Cli upload -p $Port --fqbn UNIHIKER:esp32:k10 $Sketch
if ($LASTEXITCODE -ne 0) { Write-Error "upload failed (exit $LASTEXITCODE)" }
Write-Host "[flash] OK -- board should reboot into the WASTELAND splash."
Write-Host "[flash] To monitor (interactive only):"
Write-Host "        & '$Cli' monitor -p $Port --config baudrate=115200"
