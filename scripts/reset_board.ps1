# reset_board.ps1 -- force a K10 reboot over USB serial and report its address.
#
# Why this exists: a sustained 5-bot arena load knocks the board off the
# network several times an hour (see docs/bot-testing.md "Known issues").
# Pulsing DTR/RTS is the ESP32 reset line, so this recovers it without
# physical access -- and it comes back with a clean heap, which is also the
# "power-cycle before a run you intend to trust" state that doc asks for.
#
# The address moves: it is a DHCP lease, so a reboot can land it anywhere.
# The boot log's "Boot STA connected ... ip=..." line is the only reliable
# source; do not trust a hardcoded IP in any doc.
#
# Usage: .\scripts\reset_board.ps1 [-Port COM5] [-Seconds 50]
#        prints the IP on stdout (last line) or exits 1 if it never joined.

[CmdletBinding()]
param(
  [string]$Port    = 'COM5',
  # Boot to "Boot STA connected" is ~5s on a good day, but SD mount, the
  # PSRAM web cache and the variant scan all run first and a slow SD can push
  # it well past 50s. Erring long costs nothing; erring short reports a
  # failure for a board that was merely still booting.
  [int]   $Seconds = 90
)

$ErrorActionPreference = 'Stop'

try {
  $p = New-Object System.IO.Ports.SerialPort $Port,115200,None,8,one
  $p.ReadTimeout = 500
  $p.Open()
} catch {
  Write-Error "could not open $Port : $_"
}

# DTR/RTS pulse = the ESP32 auto-reset circuit. Without it we join mid-boot
# and miss the Wi-Fi line entirely.
$p.DtrEnable = $false; $p.RtsEnable = $true;  Start-Sleep -Milliseconds 150
$p.RtsEnable = $false; $p.DtrEnable = $true;  Start-Sleep -Milliseconds 100
$p.DtrEnable = $false

$sb = New-Object System.Text.StringBuilder
$deadline = (Get-Date).AddSeconds($Seconds)
$ip = $null
while ((Get-Date) -lt $deadline) {
  try { $null = $sb.Append($p.ReadExisting()) } catch {}
  # Log lines from concurrent FreeRTOS tasks interleave, so match on the
  # whole buffer rather than per-line and take the last hit.
  $m = [regex]::Matches($sb.ToString(), 'Boot STA connected[^\r\n]*ip=(\d+\.\d+\.\d+\.\d+)')
  if ($m.Count -gt 0) { $ip = $m[$m.Count - 1].Groups[1].Value; break }
  Start-Sleep -Milliseconds 200
}
$p.Close()

if (-not $ip) {
  Write-Host "[reset] rebooted on $Port but saw no 'Boot STA connected' within ${Seconds}s."
  Write-Host "[reset] It may still be joining, or it fell back to the WASTELAND AP (192.168.4.1)."
  exit 1
}
Write-Host "[reset] board rebooted, joined Wi-Fi at $ip"
Write-Output $ip
