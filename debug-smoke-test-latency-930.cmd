@echo off
setlocal EnableExtensions EnableDelayedExpansion

REM Live latency smoke test (voice_stop_ms=930)
REM Usage:
REM   .\debug-smoke-test-latency-930.cmd [mic_selector]
REM Where mic_selector can be:
REM   - a numeric index (e.g. 0)
REM   - a name substring (e.g. Samson)
REM If omitted, uses %AI_SUBTITLER_MIC% if set, else defaults to 0.
REM Notes:
REM - Writes profile output to: debug-smoke-test-latency-930.prof.txt
REM - Uses tools\run-live-ab.ps1 (Release by default) and enables --profile

set "ROOT=%~dp0"
pushd "%ROOT%" >nul

if not "%AI_SUBTITLER_NO_SPAWN%"=="1" (
  start "Ai-Subtitler Smoke (Live Latency 930)" cmd.exe /k "%~f0 %*"
  popd >nul
  exit /b 0
)

set "MIC=%~1"
if "%MIC%"=="" if not "%AI_SUBTITLER_MIC%"=="" set "MIC=%AI_SUBTITLER_MIC%"
if "%MIC%"=="" set "MIC=0"

set "OUT=debug-smoke-test-latency-930.prof.txt"

echo Running LIVE latency smoke (stop=930ms)
echo   MIC: %MIC%
echo   OUT: %OUT%
echo(

REM Show output live in terminal for interactive feedback
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%tools\run-live-ab.ps1" -Mic "%MIC%" -OmitFeatures streamerbot,lang-fallback-fr --voice-stop-ms 930 --profile
set "ERRLVL=%errorlevel%"

echo(
REM Ctrl+C typically maps to exit code 1; treat that as OK for smoke runs.
if not "%ERRLVL%"=="0" if not "%ERRLVL%"=="1" (
  echo ERROR: Subtitler exited with code %ERRLVL%
  popd >nul
  exit /b %ERRLVL%
)

popd >nul
exit /b 0
