# _arduino-env.ps1 -- shared by build.ps1 / flash.ps1 / setup_libs.ps1 (dot-source it).
#
# Two things every arduino-cli call in this repo needs:
#
# 1. Pinned libraries. ARDUINO_DIRECTORIES_USER points arduino-cli at the
#    repo-local, gitignored .arduino\ sketchbook that setup_libs.ps1 fills
#    with exact versions. The machine-wide sketchbook (OneDrive Documents\
#    Arduino) is never searched, so a stale lib there can't leak into a build.
#
# 2. A sketch folder named after the .ino. arduino-cli refuses a sketch whose
#    folder name != main .ino name, and this repo's folder
#    (UnihikerK10MultiplayerHexMapCrawl) doesn't match Esp32HexMapCrawl.ino.
#    When they differ we build through a junction under %LOCALAPPDATA% --
#    never inside the repo, or recursive tools would loop through it. The
#    junction path is stable, so arduino-cli's build cache stays warm.

$script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$script:LibRoot  = Join-Path $RepoRoot '.arduino'
$script:Fqbn     = 'UNIHIKER:esp32:k10'

# name@version as arduino-cli's library index spells them. Bump here, then
# run setup_libs.ps1 (build.ps1 does it automatically when the stamp differs).
$script:PinnedLibs = @(
  'ESP Async WebServer@3.10.3',
  'Async TCP@3.4.10',
  'ArduinoLog@1.1.1',
  # 1.1.16, not 1.2.x: 1.2.x ships src/lgfx/v1/lv_font/font_fmt_txt.c, whose
  # lv_font_*_fmt_txt symbols collide with the core's liblvgl at link time.
  'LovyanGFX@1.1.16'
)
$script:LibStampFile = Join-Path $LibRoot 'pinned.txt'

function Use-PinnedLibs {
  $env:ARDUINO_DIRECTORIES_USER = $script:LibRoot
}

function Test-PinnedLibsCurrent {
  if (-not (Test-Path $script:LibStampFile)) { return $false }
  $want = ($script:PinnedLibs -join "`n")
  $have = (Get-Content $script:LibStampFile -Raw).Trim()
  return $have -eq $want
}

# Short alias for the platform's SDK folder, passed as compiler.sdk.path.
# The UNIHIKER platform puts ~290 "-I{compiler.sdk.path}/include/..." flags
# on every compile; at the full Arduino15 path that is ~28K characters, which
# pushes the sketch command near Windows' 32K limit. arduino-cli then rewrites
# the sketch paths as relative, the .d file's target no longer matches the
# absolute object path ("Depfile is about different object file"), and the
# sketch recompiled on every build even with nothing changed. The junction
# resolves to the very same files, so only the spelling of the paths changes.
function Get-SdkBuildProperty([string]$Cli) {
  $data = (& $Cli config get directories.data 2>$null | Out-String).Trim()
  if (-not $data) { $data = Join-Path $env:LOCALAPPDATA 'Arduino15' }
  $sdk = @(Get-ChildItem -Path "$data\packages\UNIHIKER\hardware\esp32\*\tools\sdk\esp32s3" -Directory -ErrorAction SilentlyContinue)
  if ($sdk.Count -ne 1) {
    Write-Warning "UNIHIKER SDK folder not found (or ambiguous) -- building with long SDK paths"
    return @()
  }
  $link = Join-Path $env:LOCALAPPDATA 'k10sdk'
  if (Test-Path $link) {
    $target = @((Get-Item $link -Force).Target)[0]
    if (-not $target -or ($target.TrimEnd('\') -ine $sdk[0].FullName.TrimEnd('\'))) {
      cmd /c rmdir "$link" | Out-Null   # stale junction (platform upgraded): drops the link only
    }
  }
  if (-not (Test-Path $link)) {
    New-Item -ItemType Junction -Path $link -Target $sdk[0].FullName | Out-Null
  }
  return @('--build-property', ("compiler.sdk.path=" + ($link -replace '\\', '/')))
}

# Files arduino-cli needs from the repo root: sources plus the two files the
# platform's prebuild hooks copy from the sketch folder.
$script:SketchFilePatterns = @('*.ino', '*.h', '*.hpp', '*.c', '*.cpp', 'partitions.csv')

# Returns a staged sketch folder holding only $SketchFilePatterns from the
# repo root, as hard links (so the paths in compiler errors still edit the
# real files), rebuilt on every call so git checkouts / new files are picked up.
function Resolve-SketchDir([string]$Sketch) {
  $ino = @(Get-ChildItem -Path $Sketch -Filter '*.ino' -File)
  if ($ino.Count -ne 1) { throw "expected exactly one .ino in $Sketch, found $($ino.Count)" }
  $name = [IO.Path]::GetFileNameWithoutExtension($ino[0].Name)

  # Staged even when the folder name already matches: arduino-cli copies
  # every .h/.c/.cpp/.json/.md anywhere under the sketch folder into the
  # build dir on each build -- with .arduino\ libraries, data\ and docs\ that
  # was 136 MB and ~35 s per build.
  $stageRoot = Join-Path $env:LOCALAPPDATA 'k10-sketch-stage'
  $stage = Join-Path $stageRoot $name
  if (Test-Path $stage) {
    $item = Get-Item $stage -Force
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
      cmd /c rmdir "$stage" | Out-Null   # old junction layout: drops the link only
    }
  }
  New-Item -ItemType Directory -Force -Path $stage | Out-Null
  # Never clear a folder that could resolve into the repo itself.
  if ((Get-Item $stage -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
    throw "$stage is still a link; remove it with: cmd /c rmdir `"$stage`""
  }

  foreach ($old in Get-ChildItem -Path $stage -File) {
    Remove-Item -Force $old.FullName   # a hard link: removes the link, not the repo file
  }
  $src = Get-ChildItem -Path (Join-Path $Sketch '*') -File -Include $script:SketchFilePatterns
  foreach ($f in $src) {
    $dst = Join-Path $stage $f.Name
    try {
      New-Item -ItemType HardLink -Path $dst -Target $f.FullName -ErrorAction Stop | Out-Null
    } catch {
      Copy-Item -Path $f.FullName -Destination $dst   # e.g. repo on another volume
    }
  }
  return $stage
}
