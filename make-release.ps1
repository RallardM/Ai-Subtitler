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
  "start-suspect-test.cmd",
  "start-suspect-test.ps1",
  "debug-voice-gate.cmd",
  "tune-vad-voice-thold.cmd",
  "tune-voice-stop-ms.cmd",
  "download-model-tiny-multilanguage.cmd",
  "download-model-tiny-en.cmd",
  "download-vad.cmd",
  "download-vad.ps1",
  "run.cmd",
  "run.ps1",
  "list-devices.cmd",
  "list-devices.ps1",
  "make-release.cmd",
  "make-release.ps1",
  "README.md"
)
foreach ($f in $ExtraFiles) {
  $src = Join-Path $RepoRoot $f
  if (Test-Path $src) {
    Copy-Item -Force $src $StageDir
  }
}

# Optional developer tool(s) that are still handy for validating the extracted ZIP.
$ToolsDir = Join-Path $RepoRoot "tools"
if (Test-Path $ToolsDir) {
  $StageTools = Join-Path $StageDir "tools"
  New-Item -ItemType Directory -Force $StageTools | Out-Null
  $Smoke = Join-Path $ToolsDir "smoke_test_release_wrapper.ps1"
  if (Test-Path $Smoke) {
    Copy-Item -Force $Smoke $StageTools
  }
}

New-Item -ItemType Directory -Force (Join-Path $StageDir "models") | Out-Null

# Convenience launcher in the release folder.
# Note: Windows .lnk shortcuts are not reliably portable after ZIP extraction/move
# because they tend to store/resolve absolute paths. We generate a portable .cmd
# launcher instead (double-click it).
$NormalCmd = Join-Path $StageDir "Start Ai-Subtitler (Fast).cmd"
@(
  '@echo off',
  'setlocal EnableExtensions',
  'title Ai-Subtitler Launcher (Fast)',
  'REM Batch parsing can be fragile when inspecting cmd command lines.',
  'REM To guarantee the window does not "flash and disappear" when double-clicked,',
  'REM self-spawn into cmd.exe /k unconditionally.',
  'REM Opt-out (for terminal usage): set AI_SUBTITLER_NO_SPAWN=1',
  'if "%AI_SUBTITLER_NO_SPAWN%"=="1" goto after_spawn',
  'if "%AI_SUBTITLER_SPAWNED%"=="1" goto after_spawn',
  'set "AI_SUBTITLER_SPAWNED=1"',
  'start "Ai-Subtitler (Fast)" cmd.exe /k ""%~f0" %*"',
  'exit /b 0',
  ':after_spawn',
  'set "STAMP=%~dp0launcher-stamp-start-fast.txt"',
  '>"%STAMP%" echo START %date% %time%',
  '>>"%STAMP%" echo CWD: %cd%',
  '>>"%STAMP%" echo ARGS: %*',
  'call "%~dp0start-ai-subtitler.cmd" %*',
  'set "ERR=%errorlevel%"',
  '>>"%STAMP%" echo EXIT %ERR% %date% %time%',
  'if "%ERR%"=="0" exit /b 0',
  'echo(',
  'echo Failed. errorlevel=%ERR%',
  'if "%AI_SUBTITLER_NO_PAUSE%"=="1" exit /b %ERR%',
  'echo(',
  'echo Press any key to close...',
  'pause >nul',
  'exit /b %ERR%'
) | Set-Content -Path $NormalCmd -Encoding ASCII

# Best-effort .lnk shortcut (may not remain valid if the extracted folder is moved).
try {
  $lnkPath = Join-Path $StageDir "Start Ai-Subtitler (Fast).lnk"
  $wsh = New-Object -ComObject WScript.Shell
  $s = $wsh.CreateShortcut($lnkPath)

  # Use cmd.exe (stable path) and run the portable launcher in the extracted folder.
  $s.TargetPath = "$env:SystemRoot\System32\cmd.exe"
  $s.Arguments = '/k "Start Ai-Subtitler (Fast).cmd"'
  $s.WorkingDirectory = $StageDir
  $s.IconLocation = (Join-Path $StageDir 'ai-subtitler-streamerbot.exe') + ',0'
  $s.Description = 'Ai-Subtitler (fast; prompts for mic + model)'
  $s.Save()
} catch {
  Write-Warning "Could not create .lnk shortcut: $($_.Exception.Message)"
}

if (Test-Path $ZipPath) {
  Remove-Item -Force $ZipPath
}

Compress-Archive -Path (Join-Path $StageDir "*") -DestinationPath $ZipPath

Write-Host "Created: $ZipPath"