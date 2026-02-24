@echo off
setlocal EnableExtensions EnableDelayedExpansion

REM Keep window open when double-clicked.
REM Opt-out (for terminal usage): set AI_SUBTITLER_NO_SPAWN=1
if "%AI_SUBTITLER_NO_SPAWN%"=="1" goto after_spawn
if "%AI_SUBTITLER_SPAWNED%"=="1" goto after_spawn
set "AI_SUBTITLER_SPAWNED=1"
start "Ai-Subtitler Smoke (Offline VG 930)" cmd.exe /k ""%~f0" %*"
exit /b 0
:after_spawn

REM Offline, deterministic voice-gate A/B test.
REM Writes [VG] events to a log file.

pushd "%~dp0" >nul

REM Optional: you can pass a WAV path as first arg:
REM   debug-smoke-test-offline-vg-930.cmd C:\path\to\file.wav
set "WAV=%~1"
if not "%WAV%"=="" goto have_wav

set "WAV=submodules\whisper.cpp\samples\jfk.wav"
:have_wav
if not exist "%WAV%" (
  echo ERROR: WAV not found: %WAV%
  echo(
  echo This offline test needs a WAV file.
  echo - In the repo, it defaults to: submodules\whisper.cpp\samples\jfk.wav
  echo - In the Release ZIP, that sample is NOT included.
  echo(
  echo Fix: run this script again and pass a WAV path, e.g.:
  echo   %~nx0 C:\path\to\file.wav
  popd >nul
  exit /b 1
)

set "OUT=debug-smoke-test-offline-vg-930.vg.txt"
set "ERR=debug-smoke-test-offline-vg-930.err.txt"

echo Running OFFLINE voice-gate test (stop=930ms)
echo   WAV: %WAV%
echo   OUT: %OUT%
echo   ERR: %ERR%
echo(

cmd.exe /c ""%~dp0run.cmd" --test-voice-gate "%WAV%" --vad-check-ms 50 --voice-stop-ms 930 1>"%OUT%" 2>"%ERR%""
set "ERRLVL=%errorlevel%"

echo(
if not "%ERRLVL%"=="0" (
  echo FAILED (errorlevel=%ERRLVL%^). See: %ERR%
  echo(
  echo ---- %ERR% error log ----
  type "%ERR%"
  echo ---- end error log ----
  popd >nul
  if "%AI_SUBTITLER_NO_PAUSE%"=="1" exit /b %ERRLVL%
  echo(
  echo Press any key to close...
  pause >nul
  exit /b %ERRLVL%
)

echo Done. FLUSH lines:
findstr /c:"[VG] FLUSH" "%OUT%"

popd >nul
echo(
echo SUCCESS. (Window stays open; you can close it now.)
exit /b 0
