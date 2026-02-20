param(
  [string]$ZipName = "Ai-Subtitler-Release.zip"
)

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildRelease = Join-Path $RepoRoot "build\Release"

if (-not (Test-Path $BuildRelease)) {
  throw "Release folder not found: $BuildRelease (build first)"
}

$DistRoot = Join-Path $RepoRoot "dist"
$StageDirBase = Join-Path $DistRoot "Ai-Subtitler-Release"
$StageDir = $StageDirBase
$ZipPath = Join-Path $DistRoot $ZipName

New-Item -ItemType Directory -Force $DistRoot | Out-Null
if (Test-Path $StageDir) {
  try {
    Remove-Item -Recurse -Force $StageDir
  } catch {
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $StageDir = Join-Path $DistRoot ("Ai-Subtitler-Release-" + $stamp)
    Write-Warning "Could not remove staging folder (in use): $StageDirBase"
    Write-Warning "Staging to: $StageDir"
  }
}
New-Item -ItemType Directory -Force $StageDir | Out-Null

Copy-Item -Force (Join-Path $BuildRelease "*") $StageDir

$ExtraFiles = @(
  "start-ai-subtitler.cmd",
  "download-model.cmd",
  "download-model.ps1",
  "run.cmd",
  "list-devices.cmd",
  "README.md"
)
foreach ($f in $ExtraFiles) {
  $src = Join-Path $RepoRoot $f
  if (Test-Path $src) {
    Copy-Item -Force $src $StageDir
  }
}

New-Item -ItemType Directory -Force (Join-Path $StageDir "models") | Out-Null

# Convenience launchers in the release folder.
# Note: .lnk shortcuts can be sensitive to extraction/move location on some systems,
# so we also generate a portable .cmd launcher that always works.
$FastCmd = Join-Path $StageDir "Start (Fast, Mic 0).cmd"
@(
  '@echo off',
  'setlocal EnableExtensions',
  'call "%~dp0start-ai-subtitler.cmd" 0 --fast',
  'set "ERR=%errorlevel%"',
  'if not "%ERR%"=="0" echo(',
  'if not "%ERR%"=="0" echo Failed. errorlevel=%ERR%',
  'if not "%ERR%"=="0" pause >nul',
  'exit /b %ERR%'
) | Set-Content -Path $FastCmd -Encoding ASCII

try {
  $lnkPath = Join-Path $StageDir "Start (Fast, Mic 0).lnk"
  $wsh = New-Object -ComObject WScript.Shell
  $s = $wsh.CreateShortcut($lnkPath)
  # Use a relative target so the link has the best chance of working after extraction.
  $s.TargetPath = "start-ai-subtitler.cmd"
  $s.Arguments = "0 --fast"
  $s.IconLocation = "ai-subtitler-streamerbot.exe,0"
  $s.Description = "Ai-Subtitler (fast, mic 0)"
  $s.Save()
} catch {
  Write-Warning "Could not create .lnk shortcut: $($_.Exception.Message)"
}

if (Test-Path $ZipPath) {
  Remove-Item -Force $ZipPath
}

Compress-Archive -Path (Join-Path $StageDir "*") -DestinationPath $ZipPath

Write-Host "Created: $ZipPath"