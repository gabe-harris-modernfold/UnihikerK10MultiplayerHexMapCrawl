# sync_data.ps1 -- push the data/ tree to a running K10 over Wi-Fi.
# Usage:
#   .\scripts\sync_data.ps1 192.168.4.72
#   .\scripts\sync_data.ps1 192.168.4.72 -DryRun
#   .\scripts\sync_data.ps1 localhost:8765           # mock-server target
#
# POSTs each file under data/ to http://<host>/upload?dest=/data/<rel>.
# The K10 firmware streams the body straight to the SD card and the on-device
# "FILE UPLOAD" screen (ui-upload.hpp) shows live byte counts while we run.
#
# Skip-on-hash: a manifest at data/.upload-manifest.json maps each relative
# path to its last-uploaded SHA-256 + target host. Re-runs with no changes
# upload zero files.

[CmdletBinding()]
param(
  [Parameter(Mandatory = $true, Position = 0)]
  [string]$Host_,
  [string]$DataDir,
  [string]$Manifest,
  [switch]$DryRun,
  [switch]$Force,
  [switch]$NoBuild,
  [int]$DelayMs = 500,
  [int]$Attempts = 4
)

$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'  # required when run from non-interactive PS

# $PSScriptRoot is empty inside param() defaults under `powershell -File`
# (Windows PowerShell 5.1) -- resolve paths in the body instead.
$here = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
if (-not $DataDir)  { $DataDir  = (Resolve-Path (Join-Path $here '..\data')).Path }
if (-not $Manifest) { $Manifest = Join-Path $DataDir '.upload-manifest.json' }

# Regenerate app.bundle.js(.gz) / *.gz / assets.json first so the board never
# gets sources newer than its manifest. -NoBuild skips (e.g. pushing only
# encounter JSON or images).
if (-not $NoBuild) {
  & (Join-Path $here 'build_web.ps1') -DataDir $DataDir
}

$base = "http://$Host_"
Write-Host "[sync] target : $base"
Write-Host "[sync] root   : $DataDir"
if ($DryRun) { Write-Host "[sync] DRY RUN -- no POSTs will be sent" }

# Load existing manifest (per-host hashes so re-targeting forces a re-upload).
# PS 5.1 has no -AsHashtable; convert PSCustomObject -> hashtable by hand.
function ConvertTo-HashtableDeep($obj) {
  if ($null -eq $obj) { return @{} }
  if ($obj -is [hashtable]) { return $obj }
  $h = @{}
  foreach ($p in $obj.PSObject.Properties) {
    $v = $p.Value
    if ($v -is [PSCustomObject]) { $h[$p.Name] = ConvertTo-HashtableDeep $v }
    else                          { $h[$p.Name] = $v }
  }
  return $h
}

$state = @{}
if ((Test-Path $Manifest) -and -not $Force) {
  try {
    $raw = Get-Content $Manifest -Raw | ConvertFrom-Json
    $state = ConvertTo-HashtableDeep $raw
  } catch { $state = @{} }
}
if (-not $state.ContainsKey($Host_)) { $state[$Host_] = @{} }

$files = Get-ChildItem -LiteralPath $DataDir -File -Recurse |
         Where-Object { $_.Name -ne '.upload-manifest.json' }

$pushed  = 0
$skipped = 0
$failed  = 0

foreach ($f in $files) {
  $rel = $f.FullName.Substring($DataDir.Length).TrimStart('\','/').Replace('\','/')
  $hash = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash
  $prev = $state[$Host_][$rel]

  if ($prev -eq $hash -and -not $Force) {
    $skipped++
    continue
  }

  $url = "$base/upload?dest=/data/$rel"
  $sz  = $f.Length
  Write-Host ("  {0,7} bytes  {1}" -f $sz, $rel)

  if (-not $DryRun) {
    # The firmware answers 500 when the SD write failed or came up short
    # (intermittent — SD/SPI contention with the LCD task). A retry rewrites
    # the whole file, so a truncated first attempt is harmless.
    $ok = $false
    for ($attempt = 1; $attempt -le $Attempts -and -not $ok; $attempt++) {
      try {
        $resp = Invoke-WebRequest -Uri $url -Method Post -InFile $f.FullName `
                                  -ContentType 'application/octet-stream' `
                                  -UseBasicParsing -TimeoutSec 30
        # Firmware replies "OK <bytes written>"; the mock replies "OK". Treat a
        # count that disagrees with the local size as a failed upload.
        $m = [regex]::Match([string]$resp.Content, '^OK(?:\s+(\d+))?')
        if (-not $m.Success) { throw "unexpected reply: $($resp.Content)" }
        if ($m.Groups[1].Success -and [int64]$m.Groups[1].Value -ne $sz) {
          throw "board wrote $($m.Groups[1].Value) of $sz bytes"
        }
        $ok = $true
      } catch {
        if ($attempt -lt $Attempts) {
          Write-Host ("          retry {0}/{1} {2} : {3}" -f $attempt, ($Attempts - 1), $rel, $_.Exception.Message)
          Start-Sleep -Milliseconds ($DelayMs * 2 * $attempt)
        } else {
          Write-Warning "FAIL $rel : $($_.Exception.Message)"
        }
      }
    }
    if ($ok) {
      $state[$Host_][$rel] = $hash
      $pushed++
      Start-Sleep -Milliseconds $DelayMs
    } else {
      $failed++
    }
  } else {
    $pushed++
  }
}

if (-not $DryRun -and $pushed -gt 0) {
  $state | ConvertTo-Json -Depth 5 | Set-Content -Path $Manifest -Encoding utf8
}

Write-Host ""
Write-Host ("[sync] {0} pushed, {1} skipped, {2} failed" -f $pushed, $skipped, $failed)
if ($failed -gt 0) { exit 1 }
