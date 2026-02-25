@echo off
setlocal EnableExtensions EnableDelayedExpansion

REM Prevent recursive spawning of new windows
if "%AI_SUBTITLER_NO_SPAWN%"=="1" goto :skip_spawn
set "AI_SUBTITLER_NO_SPAWN=1"
start "Ai-Subtitler Live Latency A/B" cmd.exe /k "%~f0 %*"
exit /b 0
:skip_spawn

REM Guided Live Latency A/B test (930 vs 650)
REM Usage:
REM   .\debug-live-latency-ab.cmd [mic_selector]
REM Where mic_selector can be:
REM   - a numeric index (e.g. 0)
REM   - a name substring (e.g. Samson)
REM If omitted, uses %AI_SUBTITLER_MIC% if set, else defaults to Samson.
REM
REM Notes:
REM - Runs the existing smoke scripts sequentially.
REM - Each smoke run is ended with Ctrl+C when you're done speaking.
REM - Produces/updates:
REM     debug-smoke-test-latency-930.prof.txt
REM     debug-smoke-test-latency-650.prof.txt

set "ROOT=%~dp0"
pushd "%ROOT%" >nul

set "MIC=%~1"
if "%MIC%"=="" if not "%AI_SUBTITLER_MIC%"=="" set "MIC=%AI_SUBTITLER_MIC%"
if "%MIC%"=="" set "MIC=Samson"

echo =========================================
echo Live Latency A/B (930 vs 650)
echo Mic selector: %MIC%
echo.
echo Run 1/2: stop=930ms
echo - Speak a few short sentences.
echo - Press Ctrl+C when done.
echo =========================================
echo.

call "%ROOT%debug-smoke-test-latency-930.cmd" "%MIC%"
set "ERR1=%errorlevel%"
if not "%ERR1%"=="0" (
  echo(
  echo ABORT: 930 smoke failed (errorlevel=%ERR1%^")
  popd >nul
  exit /b %ERR1%
)

echo.
echo =========================================
echo Run 2/2: stop=650ms
echo - Speak the same way as Run 1.
echo - Press Ctrl+C when done.
echo =========================================
echo.

call "%ROOT%debug-smoke-test-latency-650.cmd" "%MIC%"
set "ERR2=%errorlevel%"
if not "%ERR2%"=="0" (
  echo(
  echo ABORT: 650 smoke failed (errorlevel=%ERR2%^")
  popd >nul
  exit /b %ERR2%
)

echo.
echo =========================================
echo Combined Summary (930 vs 650)
echo =========================================
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%tools\parse-profile.ps1" "%ROOT%debug-smoke-test-latency-930.prof.txt" "%ROOT%debug-smoke-test-latency-650.prof.txt"

popd >nul
echo(
echo SUCCESS. (Window stays open; you can close it now.)
exit /b 0
