# build_web.ps1 -- produce the K10's web-asset bundle from data/web-assets.json.
# Usage:
#   .\scripts\build_web.ps1            # write data/app.bundle.js(.gz), *.gz, assets.json
#   .\scripts\build_web.ps1 -Check     # exit 1 if outputs are missing/stale (CI-ish)
#
# Outputs (all gitignored, all under data/ so sync_data.ps1 and the USB-MSC
# bulk copy pick them up automatically):
#   app.bundle.js      concatenation of web-assets.json "scripts", in order
#   app.bundle.js.gz   gzip of the above (firmware prefers .gz siblings)
#   style.css.gz, index.html.gz
#   assets.json        manifest the browser loader fetches first:
#                      { version, generated, styles:[{url,size,gz}], scripts:[...] }
#
# The version is the first 12 hex chars of SHA-256 over every input file, so it
# only changes when content changes -- the client appends ?v=<version> to every
# asset URL, which is what makes the firmware's immutable Cache-Control safe.
#
# Why concatenate instead of shipping ~18 script files: each HTTP request to
# the K10 is a fresh TCP connection through AsyncTCP; fewer, gzipped requests
# is the single biggest reduction in load on the board (see docs/dev-loop.md
# "Web asset pipeline").

[CmdletBinding()]
param(
  [string]$DataDir,
  [switch]$Check
)

$ErrorActionPreference = 'Stop'

if (-not $DataDir) {
  # $PSScriptRoot is empty in some invocation modes (powershell -File from a
  # non-interactive host) -- fall back to the invocation path.
  $here = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
  $DataDir = (Resolve-Path (Join-Path $here '..\data')).Path
}

function Read-Bytes([string]$p) { [System.IO.File]::ReadAllBytes($p) }
function Write-Bytes([string]$p, [byte[]]$b) { [System.IO.File]::WriteAllBytes($p, $b) }

function Gzip-Bytes([byte[]]$raw) {
  $ms = New-Object System.IO.MemoryStream
  $gz = New-Object System.IO.Compression.GZipStream($ms, [System.IO.Compression.CompressionLevel]::Optimal, $true)
  $gz.Write($raw, 0, $raw.Length)
  $gz.Dispose()
  $out = $ms.ToArray()
  $ms.Dispose()
  # .NET writes MTIME=0 in the gzip header, so output is deterministic for
  # identical input -- sync_data.ps1's hash-skip keeps working.
  return $out
}

$srcPath = Join-Path $DataDir 'web-assets.json'
if (-not (Test-Path $srcPath)) { Write-Error "missing $srcPath" }
$src = Get-Content $srcPath -Raw | ConvertFrom-Json
$scripts = @($src.scripts)
$styles  = @($src.styles)
if ($scripts.Count -eq 0) { Write-Error "web-assets.json has no scripts" }

# ── Gather inputs + running hash ─────────────────────────────────────────────
$sha = [System.Security.Cryptography.SHA256]::Create()
$utf8 = New-Object System.Text.UTF8Encoding($false)
$bundleParts = New-Object System.Collections.Generic.List[byte[]]
$missing = @()

foreach ($s in $scripts) {
  $p = Join-Path $DataDir $s
  if (-not (Test-Path $p)) { $missing += $s; continue }
  $bytes = Read-Bytes $p
  $null = $sha.TransformBlock($bytes, 0, $bytes.Length, $null, 0)
  $bundleParts.Add($utf8.GetBytes("`n;/* ---- $s ---- */`n"))
  $bundleParts.Add($bytes)
  $bundleParts.Add($utf8.GetBytes("`n"))
}
foreach ($c in $styles) {
  $p = Join-Path $DataDir $c
  if (-not (Test-Path $p)) { $missing += $c; continue }
  $bytes = Read-Bytes $p
  $null = $sha.TransformBlock($bytes, 0, $bytes.Length, $null, 0)
}
$indexPath = Join-Path $DataDir 'index.html'
if (Test-Path $indexPath) {
  $bytes = Read-Bytes $indexPath
  $null = $sha.TransformBlock($bytes, 0, $bytes.Length, $null, 0)
} else { $missing += 'index.html' }

if ($missing.Count -gt 0) { Write-Error ("web-assets.json references missing files: " + ($missing -join ', ')) }

$null = $sha.TransformFinalBlock([byte[]]@(), 0, 0)
$version = ([System.BitConverter]::ToString($sha.Hash) -replace '-', '').Substring(0, 12).ToLower()
$sha.Dispose()

# ── Bundle ──────────────────────────────────────────────────────────────────
$total = 0; foreach ($b in $bundleParts) { $total += $b.Length }
$bundle = New-Object byte[] $total
$off = 0
foreach ($b in $bundleParts) { [Array]::Copy($b, 0, $bundle, $off, $b.Length); $off += $b.Length }
$bundleGz = Gzip-Bytes $bundle

$styleEntries = @()
$styleOutputs = @{}
foreach ($c in $styles) {
  $plain = Read-Bytes (Join-Path $DataDir $c)
  $gz    = Gzip-Bytes $plain
  $styleOutputs[$c] = $gz
  $styleEntries += [ordered]@{ url = $c; size = $plain.Length; gz = $gz.Length }
}
$indexGz = Gzip-Bytes (Read-Bytes $indexPath)

$manifest = [ordered]@{
  version   = $version
  generated = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
  mode      = 'bundle'
  styles    = $styleEntries
  scripts   = @([ordered]@{ url = 'app.bundle.js'; size = $bundle.Length; gz = $bundleGz.Length; sources = $scripts })
}
$manifestJson = ($manifest | ConvertTo-Json -Depth 5) + "`n"

# ── Check / write ───────────────────────────────────────────────────────────
$outManifest = Join-Path $DataDir 'assets.json'
if ($Check) {
  $stale = $true
  if (Test-Path $outManifest) {
    try { $stale = ((Get-Content $outManifest -Raw | ConvertFrom-Json).version -ne $version) } catch { $stale = $true }
  }
  if ($stale) { Write-Host "[build_web] STALE (want version $version)"; exit 1 }
  Write-Host "[build_web] up to date ($version)"; exit 0
}

Write-Bytes (Join-Path $DataDir 'app.bundle.js')    $bundle
Write-Bytes (Join-Path $DataDir 'app.bundle.js.gz') $bundleGz
foreach ($c in $styles) { Write-Bytes (Join-Path $DataDir "$c.gz") $styleOutputs[$c] }
Write-Bytes (Join-Path $DataDir 'index.html.gz') $indexGz
[System.IO.File]::WriteAllText($outManifest, $manifestJson, $utf8)

Write-Host ("[build_web] version {0}" -f $version)
Write-Host ("[build_web] app.bundle.js  {0,8} B  -> {1,7} B gz  ({2} files)" -f $bundle.Length, $bundleGz.Length, $scripts.Count)
foreach ($e in $styleEntries) { Write-Host ("[build_web] {0,-14} {1,8} B  -> {2,7} B gz" -f $e.url, $e.size, $e.gz) }
Write-Host ("[build_web] index.html     {0,8} B  -> {1,7} B gz" -f (Get-Item $indexPath).Length, $indexGz.Length)
