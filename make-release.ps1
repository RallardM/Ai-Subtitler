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
  "debug-voice-gate.cmd",
  "download-model-tiny-multilanguage.cmd",
  "download-model-tiny-en.cmd",
  "download-vad.cmd",
  "download-vad.ps1",
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
# Note: Windows .lnk shortcuts are not reliably portable after ZIP extraction/move
# because they tend to store/resolve absolute paths. We generate a portable .cmd
# launcher instead (double-click it).
$FastCmd = Join-Path $StageDir "Start (Fast, Mic 0).cmd"
@(
  '@echo off',
  'setlocal EnableExtensions',
  'title Ai-Subtitler Launcher (Fast, Mic 0)',
  'REM Batch parsing can be fragile when inspecting cmd command lines.',
  'REM To guarantee the window does not "flash and disappear" when double-clicked,',
  'REM self-spawn into cmd.exe /k unconditionally.',
  'REM Opt-out (for terminal usage): set AI_SUBTITLER_NO_SPAWN=1',
  'if "%AI_SUBTITLER_NO_SPAWN%"=="1" goto after_spawn',
  'if "%AI_SUBTITLER_SPAWNED%"=="1" goto after_spawn',
  'set "AI_SUBTITLER_SPAWNED=1"',
  'start "Ai-Subtitler (Fast, Mic 0)" cmd.exe /k ""%~f0" %*"',
  'exit /b 0',
  ':after_spawn',
  'set "STAMP=%~dp0launcher-stamp-start-fast-mic0.txt"',
  '>"%STAMP%" echo START %date% %time%',
  '>>"%STAMP%" echo CWD: %cd%',
  '>>"%STAMP%" echo ARGS: %*',
  'call "%~dp0start-ai-subtitler.cmd" 0 --fast %*',
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
) | Set-Content -Path $FastCmd -Encoding ASCII

$TraceCmd = Join-Path $StageDir "Start (Fast, Prompt Mic, Trace Voice Gate).cmd"
@(
  '@echo off',
  'setlocal EnableExtensions',
  'title Ai-Subtitler Launcher (Fast, Trace Voice Gate)',
  'REM Batch parsing can be fragile when inspecting cmd command lines.',
  'REM To guarantee the window does not "flash and disappear" when double-clicked,',
  'REM self-spawn into cmd.exe /k unconditionally.',
  'REM Opt-out (for terminal usage): set AI_SUBTITLER_NO_SPAWN=1',
  'if "%AI_SUBTITLER_NO_SPAWN%"=="1" goto after_spawn',
  'if "%AI_SUBTITLER_SPAWNED%"=="1" goto after_spawn',
  'set "AI_SUBTITLER_SPAWNED=1"',
  'start "Ai-Subtitler (Fast, Trace Voice Gate)" cmd.exe /k ""%~f0" %*"',
  'exit /b 0',
  ':after_spawn',
  'set "STAMP=%~dp0launcher-stamp-start-fast-trace-voice-gate.txt"',
  '>"%STAMP%" echo START %date% %time%',
  '>>"%STAMP%" echo CWD: %cd%',
  '>>"%STAMP%" echo ARGS: %*',
  'call "%~dp0start-ai-subtitler.cmd" --fast --trace-voice-gate %*',
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
) | Set-Content -Path $TraceCmd -Encoding ASCII

$NoGateCmd = Join-Path $StageDir "Start (Fast, Prompt Mic, No Voice Gate).cmd"
@(
  '@echo off',
  'setlocal EnableExtensions',
  'title Ai-Subtitler Launcher (Fast, No Voice Gate)',
  'REM Batch parsing can be fragile when inspecting cmd command lines.',
  'REM To guarantee the window does not "flash and disappear" when double-clicked,',
  'REM self-spawn into cmd.exe /k unconditionally.',
  'REM Opt-out (for terminal usage): set AI_SUBTITLER_NO_SPAWN=1',
  'if "%AI_SUBTITLER_NO_SPAWN%"=="1" goto after_spawn',
  'if "%AI_SUBTITLER_SPAWNED%"=="1" goto after_spawn',
  'set "AI_SUBTITLER_SPAWNED=1"',
  'start "Ai-Subtitler (Fast, No Voice Gate)" cmd.exe /k ""%~f0" %*"',
  'exit /b 0',
  ':after_spawn',
  'set "STAMP=%~dp0launcher-stamp-start-fast-no-voice-gate.txt"',
  '>"%STAMP%" echo START %date% %time%',
  '>>"%STAMP%" echo CWD: %cd%',
  '>>"%STAMP%" echo ARGS: %*',
  'call "%~dp0start-ai-subtitler.cmd" --no-voice-gate %*',
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
) | Set-Content -Path $NoGateCmd -Encoding ASCII

# Best-effort .lnk shortcuts (may not remain valid if the extracted folder is moved).
try {
  $lnkPath = Join-Path $StageDir "Start (Fast, Prompt Mic, Trace Voice Gate).lnk"
  $wsh = New-Object -ComObject WScript.Shell
  $s = $wsh.CreateShortcut($lnkPath)

  # Use cmd.exe (stable path) and run the portable launcher in the extracted folder.
  $s.TargetPath = "$env:SystemRoot\System32\cmd.exe"
  $s.Arguments = '/k "Start (Fast, Prompt Mic, Trace Voice Gate).cmd"'
  $s.WorkingDirectory = $StageDir
  $s.IconLocation = (Join-Path $StageDir 'ai-subtitler-streamerbot.exe') + ',0'
  $s.Description = 'Ai-Subtitler (fast, trace voice gate; prompts for mic)'
  $s.Save()
} catch {
  Write-Warning "Could not create .lnk shortcut: $($_.Exception.Message)"
}

if (Test-Path $ZipPath) {
  Remove-Item -Force $ZipPath
}

Compress-Archive -Path (Join-Path $StageDir "*") -DestinationPath $ZipPath

Write-Host "Created: $ZipPath"