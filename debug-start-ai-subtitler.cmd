@echo off
setlocal EnableExtensions EnableDelayedExpansion

REM Debug launcher shortcut.
REM - Prefers Debug build (via AI_SUBTITLER_CONFIG)
REM - Enables built-in timing output (--profile)
REM - Keeps the window open when double-clicked (self-spawn into cmd.exe /k)

REM Self-spawn into a persistent window (same pattern as debug-voice-gate.cmd)
REM Opt-out (for terminal usage): set AI_SUBTITLER_NO_SPAWN=1
if "%AI_SUBTITLER_NO_SPAWN%"=="1" goto after_spawn
if "%AI_SUBTITLER_SPAWNED%"=="1" goto after_spawn
set "AI_SUBTITLER_SPAWNED=1"
start "Ai-Subtitler Debug" cmd.exe /k ""%~f0" %*"
exit /b 0
:after_spawn

set "AI_SUBTITLER_CONFIG=Debug"

REM Mic convenience:
REM   debug-start-ai-subtitler.cmd 0
REM   debug-start-ai-subtitler.cmd Samson
REM   debug-start-ai-subtitler.cmd --mic "Microphone (Samson Q1U)"

set "MIC_INDEX="
set "MIC_SUBSTR="
set "EXTRA_ARGS="

if "%~1"=="" goto run

REM Pass through explicit options.
if "%~1:~0,1%"=="-" goto collect

REM If first arg is an integer, treat it as positional mic index.
set "_first=%~1"
for /f "delims=0123456789" %%Z in ("%_first%") do set "_nondigit=%%Z"
if not defined _nondigit (
  set "MIC_INDEX=%~1"
  shift
  goto collect
)

REM Otherwise treat it as mic name substring.
set "MIC_SUBSTR=%~1"
shift

goto collect

:collect
if "%~1"=="" goto run
set "EXTRA_ARGS=%EXTRA_ARGS% %1"
shift
goto collect

:run
if defined MIC_INDEX (
  call "%~dp0start-ai-subtitler.cmd" --keep-open %MIC_INDEX% --profile %EXTRA_ARGS%
) else if defined MIC_SUBSTR (
  call "%~dp0start-ai-subtitler.cmd" --keep-open --mic "%MIC_SUBSTR%" --profile %EXTRA_ARGS%
) else (
  call "%~dp0start-ai-subtitler.cmd" --keep-open --profile %EXTRA_ARGS%
)
exit /b %errorlevel%
